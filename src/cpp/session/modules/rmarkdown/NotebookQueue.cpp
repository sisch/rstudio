/*
 * NotebookQueue.cpp
 *
 * Copyright (C) 2009-16 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionRmdNotebook.hpp"
#include "NotebookQueue.hpp"
#include "NotebookQueueUnit.hpp"
#include "NotebookExec.hpp"
#include "NotebookDocQueue.hpp"
#include "NotebookCache.hpp"
#include "NotebookAlternateEngines.hpp"

#include <boost/foreach.hpp>

#include <r/RInterface.hpp>
#include <r/RExec.hpp>

#include <core/Exec.hpp>
#include <core/Thread.hpp>

#include <session/SessionModuleContext.hpp>
#include <session/SessionClientEvent.hpp>
#include <session/SessionClientEventService.hpp>
#include <session/http/SessionRequest.hpp>

#define kThreadQuitCommand "thread_quit"

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {
namespace {

enum ChunkExecState
{
   ChunkExecStarted   = 0,
   ChunkExecFinished  = 1,
   ChunkExecCancelled = 2
};

// represents the global queue of work 
class NotebookQueue : boost::noncopyable
{
public:
   NotebookQueue() 
   {
      // launch a thread to process console input
      pInput_ = 
         boost::make_shared<core::thread::ThreadsafeQueue<std::string> >();
      thread::safeLaunchThread(boost::bind(
               &NotebookQueue::consoleThreadMain, this), &console_);

      // register handler for chunk exec complete
      handlers_.push_back(events().onChunkExecCompleted.connect(
               boost::bind(&NotebookQueue::onChunkExecCompleted, this, _1, _2, _3)));
   }

   ~NotebookQueue()
   {
      // let thread clean up asynchronously
      pInput_->enque(kThreadQuitCommand);

      // unregister handlers
      BOOST_FOREACH(boost::signals::connection connection, handlers_)
      {
         connection.disconnect();
      }
   }

   bool complete()
   {
      return queue_.empty();
   }

   Error process(ExpressionMode mode)
   {
      // if list is empty, we're done
      if (queue_.empty())
         return Success();

      // defer if R is currently executing code (we'll initiate processing when
      // the console continues)
      if (r::getGlobalContext()->nextcontext != NULL)
         return Success();

      // if we have a currently executing unit, execute it; otherwise, pop the
      // next unit off the stack
      if (execUnit_)
      {
         if (execContext_ && execContext_->hasErrors())
         {
            // when an error occurs, see what the chunk options say; if they
            // have error = TRUE we can keep going, but in all other
            // circumstances we should stop right away
            const json::Object& options = execContext_->options();
            bool error = false;
            json::readObject(options, "error", &error);
            if (!error)
            {
               clear();
               return Success();
            }
         }
         if (execUnit_->complete())
         {
            bool incomplete = false;

            // if we're still in continuation mode but we're at the end of the
            // chunk, generate an error
            if (mode == ExprModeContinuation &&
                execUnit_->execScope() == ExecScopeChunk)
            {
               incomplete = true;
               sendIncompleteError(execUnit_);
            }

            // unit has finished executing; remove it from the queue
            popUnit(execUnit_);

            // notify client
            enqueueExecStateChanged(ChunkExecFinished, execContext_ ?
                  execContext_->options() : json::Object());

            // clean up current exec unit 
            if (execContext_)
            {
               execContext_->disconnect();
               execContext_.reset();
            }
            execUnit_.reset();

            // if the unit was incomplete, we need to wait for the interrupt
            // to complete before we execute more code
            if (incomplete)
               return Success();
         }
         else
            return executeCurrentUnit(mode);
      }

      return executeNextUnit(mode);
   }

   Error update(boost::shared_ptr<NotebookQueueUnit> pUnit, QueueOperation op, 
      const std::string& before)
   {
      // find the document queue corresponding to this unit
      BOOST_FOREACH(const boost::shared_ptr<NotebookDocQueue> queue, queue_)
      {
         if (queue->docId() == pUnit->docId())
         {
            queue->update(pUnit, op, before);
            break;
         }
      }

      return Success();
   }

   void add(boost::shared_ptr<NotebookDocQueue> pQueue)
   {
      queue_.push_back(pQueue);
   }

   void clear()
   {
      // clean up any active execution context
      if (execContext_)
         execContext_->disconnect();
      if (execUnit_)
         execUnit_.reset();

      // remove all document queues
      queue_.clear();
   }

   json::Value getDocQueue(const std::string& docId)
   {
      BOOST_FOREACH(boost::shared_ptr<NotebookDocQueue> pQueue, queue_)
      {
         if (pQueue->docId() == docId)
            return pQueue->toJson();
      }
      return json::Value();
   }

   void onConsolePrompt(const std::string& prompt)
   {
      Error error = process(prompt == "+ " ? ExprModeContinuation : 
                                             ExprModeNew);
      if (error)
         LOG_ERROR(error);
   }

private:

   void onChunkExecCompleted(const std::string& docId, 
         const std::string& chunkId, const std::string& nbCtxId)
   {
      if (!execUnit_)
         return;

      // if this is the currently executing chunk but it doesn't have an R
      // execution context, it must be executing with an alternate engine; 
      // this event signals that the alternate engine is finished, so move to
      // the next document in the queue
      if (execUnit_->docId() == docId && execUnit_->chunkId() == chunkId &&
          !execContext_)
      {
         // remove from the queue
         popUnit(execUnit_);

         // signal client
         enqueueExecStateChanged(ChunkExecFinished, json::Object());
         
         // execute the next chunk, if any
         execUnit_.reset();
         process(ExprModeNew);
      }
   }

   // execute the next line or expression in the current execution unit
   Error executeCurrentUnit(ExpressionMode mode)
   {
      // ensure we have a unit to execute 
      if (!execUnit_)
         return Success();

      // if this isn't the continuation of an expression, perform any
      // post-expression operations
      if (mode == ExprModeNew && execContext_)
         execContext_->onExprComplete();
         
      ExecRange range;
      std::string code = execUnit_->popExecRange(&range, mode); 
      sendConsoleInput(execUnit_->chunkId(), code);

      // let client know the range has been sent to R
      json::Object exec;
      exec["doc_id"]     = execUnit_->docId();
      exec["chunk_id"]   = execUnit_->chunkId();
      exec["exec_range"] = range.toJson();
      exec["expr_mode"]  = mode;
      exec["code"]       = code;
      module_context::enqueClientEvent(
            ClientEvent(client_events::kNotebookRangeExecuted, exec));

      return Success();
   }

   void sendConsoleInput(const std::string& chunkId, const json::Value& input)
   {
      json::Array arr;
      ExecRange range(0, 0);
      arr.push_back(input);
      arr.push_back(chunkId);

      // formulate request body
      json::Object rpc;
      rpc["method"] = "console_input";
      rpc["params"] = arr;
      rpc["clientId"] = clientEventService().clientId();

      // serialize RPC body and send it to helper thread for submission
      std::ostringstream oss;
      json::write(rpc, oss);
      pInput_->enque(oss.str());
   }

   Error executeNextUnit(ExpressionMode mode)
   {
      // no work to do if we have no documents
      if (queue_.empty())
         return Success();

      // get the next execution unit from the current queue
      boost::shared_ptr<NotebookDocQueue> docQueue = *queue_.begin();
      if (docQueue->complete())
         return Success();

      boost::shared_ptr<NotebookQueueUnit> unit = docQueue->firstUnit();

      // establish execution context for the unit
      json::Object options;
      Error error = unit->parseOptions(&options);
      if (error)
         LOG_ERROR(error);

      // in batch mode, make sure unit should be evaluated -- note that
      // eval=FALSE units generally do not get sent up in the first place, so
      // if we're here it's because the unit has eval=<expr>
      if (unit->execMode() == ExecModeBatch)
      {
         bool eval = true;
         json::readObject(options, "eval", &eval);
         if (!eval)
         {
            return skipUnit();
         }
      }

      // compute context
      std::string ctx = docQueue->commitMode() == ModeCommitted ?
         kSavedCtx : notebookCtxId();

      // compute engine
      std::string engine = "r";
      json::readObject(options, "engine", &engine);
      if (engine == "r")
      {
         execContext_ = boost::make_shared<ChunkExecContext>(
            unit->docId(), unit->chunkId(), ctx, unit->execScope(), options,
            docQueue->pixelWidth(), docQueue->charWidth());
         execContext_->connect();
         execUnit_ = unit;
         enqueueExecStateChanged(ChunkExecStarted, options);
      }
      else
      {
         // execute with alternate engine
         std::string innerCode;
         error = unit->innerCode(&innerCode);
         if (error)
         {
            LOG_ERROR(error);
         }
         else
         {
            execUnit_ = unit;
            enqueueExecStateChanged(ChunkExecStarted, options);
            error = executeAlternateEngineChunk(
               unit->docId(), unit->chunkId(), ctx, engine, innerCode, options);
            if (error)
               LOG_ERROR(error);
         }
      }

      // if there was an error, skip the chunk
      if (error)
         return skipUnit();

      if (engine == "r")
      {
         error = executeCurrentUnit(ExprModeNew);
         if (error)
            LOG_ERROR(error);
      }

      return Success();
   }

   // main function for thread which receives console input
   void consoleThreadMain()
   {
      // create our own reference to the threadsafe queue (this prevents it 
      // from getting cleaned up when the parent detaches)
      boost::shared_ptr<core::thread::ThreadsafeQueue<std::string> > pInput = 
         pInput_;

      std::string input;
      while (pInput->deque(&input, boost::posix_time::not_a_date_time))
      {
         // if we were asked to quit, stop processing now
         if (input == kThreadQuitCommand)
            return;

         // loop back console input request to session -- this allows us to treat 
         // notebook console input exactly as user console input
         core::http::Response response;
         Error error = session::http::sendSessionRequest(
               "/rpc/console_input", input, &response);
         if (error)
            LOG_ERROR(error);
      }
   }

   void enqueueExecStateChanged(ChunkExecState state, 
         const json::Object& options)
   {
      json::Object event;
      event["doc_id"]     = execUnit_->docId();
      event["chunk_id"]   = execUnit_->chunkId();
      event["exec_state"] = state;
      event["options"]    = options;
      module_context::enqueClientEvent(ClientEvent(
               client_events::kChunkExecStateChanged, event));
   }

   Error skipUnit()
   {
      if (queue_.empty())
         return Success();

      boost::shared_ptr<NotebookDocQueue> docQueue = *queue_.begin();
      if (docQueue->complete())
         return Success();

      boost::shared_ptr<NotebookQueueUnit> unit = docQueue->firstUnit();
      popUnit(unit);

      execUnit_ = unit;
      enqueueExecStateChanged(ChunkExecCancelled, json::Object());

      return executeNextUnit(ExprModeNew);
   }

   void popUnit(boost::shared_ptr<NotebookQueueUnit> pUnit)
   {
      if (queue_.empty())
         return;

      // remove this unit from the queue
      boost::shared_ptr<NotebookDocQueue> docQueue = *queue_.begin();
      docQueue->update(pUnit, QueueDelete, "");

      // advance if queue is complete
      if (docQueue->complete())
         queue_.pop_front();
   }

   void sendIncompleteError(boost::shared_ptr<NotebookQueueUnit> unit)
   {
      // raise an error
      r::exec::error("Incomplete expression: " + unit->executingCode());

      // send an interrupt to the console to abort the unterminated 
      // expression
      sendConsoleInput(execUnit_->chunkId(), json::Value());
   }

   // the documents with active queues
   std::list<boost::shared_ptr<NotebookDocQueue> > queue_;

   // the execution context for the currently executing chunk
   boost::shared_ptr<NotebookQueueUnit> execUnit_;
   boost::shared_ptr<ChunkExecContext> execContext_;

   // registered signal handlers
   std::vector<boost::signals::connection> handlers_;

   // the thread which submits console input, and the queue which feeds it
   boost::thread console_;
   boost::shared_ptr<core::thread::ThreadsafeQueue<std::string> > pInput_;
};

static boost::shared_ptr<NotebookQueue> s_queue;

Error updateExecQueue(const json::JsonRpcRequest& request,
                      json::JsonRpcResponse* pResponse)
{
   json::Object unitJson;
   int op = 0;
   std::string before;
   Error error = json::readParams(request.params, &unitJson, &op, &before);
   if (error)
      return error;

   boost::shared_ptr<NotebookQueueUnit> pUnit = 
      boost::make_shared<NotebookQueueUnit>();
   error = NotebookQueueUnit::fromJson(unitJson, &pUnit);
   if (error)
      return error;
   if (!s_queue)
      return Success();

   return s_queue->update(pUnit, static_cast<QueueOperation>(op), before);
}

Error executeNotebookChunks(const json::JsonRpcRequest& request,
                            json::JsonRpcResponse* pResponse)
{
   json::Object docObj;
   Error error = json::readParams(request.params, &docObj);

   boost::shared_ptr<NotebookDocQueue> pQueue = 
      boost::make_shared<NotebookDocQueue>();
   error = NotebookDocQueue::fromJson(docObj, &pQueue);
   if (error)
      return error;

   // create queue if it doesn't exist
   if (!s_queue)
      s_queue = boost::make_shared<NotebookQueue>();

   // add the queue and process immediately
   s_queue->add(pQueue);
   s_queue->process(ExprModeNew);

   return Success();
}

void onConsolePrompt(const std::string& prompt)
{
   if (s_queue)
   {
      s_queue->onConsolePrompt(prompt);
   }

   // clean up queue if it's finished executing
   if (s_queue && s_queue->complete())
   {
      s_queue.reset();
   }
}

void onUserInterrupt()
{
   if (s_queue)
   {
      s_queue->clear();
      s_queue.reset();
   }
}

} // anonymous namespace

json::Value getDocQueue(const std::string& docId)
{
   if (!s_queue)
      return json::Value();

   return s_queue->getDocQueue(docId);
}

Error initQueue()
{
   using boost::bind;
   using namespace module_context;

   module_context::events().onConsolePrompt.connect(onConsolePrompt);
   module_context::events().onUserInterrupt.connect(onUserInterrupt);

   ExecBlock initBlock;
   initBlock.addFunctions()
      (bind(registerRpcMethod, "update_notebook_exec_queue", updateExecQueue))
      (bind(registerRpcMethod, "execute_notebook_chunks", executeNotebookChunks));

   return initBlock.execute();
}

} // namespace notebook
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio