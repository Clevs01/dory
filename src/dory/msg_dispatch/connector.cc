/* <dory/msg_dispatch/connector.cc>

   ----------------------------------------------------------------------------
   Copyright 2013-2014 if(we)
   Copyright 2015 Dave Peterson <dave@dspeterson.com>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
   ----------------------------------------------------------------------------

   Implements <dory/msg_dispatch/connector.h>.
 */

#include <dory/msg_dispatch/connector.h>

#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <base/error_utils.h>
#include <base/gettid.h>
#include <base/no_default_case.h>
#include <base/on_destroy.h>
#include <base/time_util.h>
#include <dory/kafka_proto/request_response.h>
#include <dory/msg_dispatch/produce_response_processor.h>
#include <dory/msg_state_tracker.h>
#include <dory/util/connect_to_host.h>
#include <dory/util/system_error_codes.h>
#include <dory/util/time_util.h>
#include <server/counter.h>
#include <socket/db/error.h>

#include <dory/msg_dispatch/common.h>
#include <server/counter.h>

using namespace Base;
using namespace Dory;
using namespace Dory::Batch;
using namespace Dory::Debug;
using namespace Dory::KafkaProto;
using namespace Dory::KafkaProto::Produce;
using namespace Dory::MsgDispatch;
using namespace Dory::Util;

SERVER_COUNTER(AckNotRequired);
SERVER_COUNTER(BadProduceResponse);
SERVER_COUNTER(BadProduceResponseSize);
SERVER_COUNTER(BugProduceRequestEmpty);
SERVER_COUNTER(ConnectorCheckInputQueue);
SERVER_COUNTER(ConnectorCleanupAfterJoin);
SERVER_COUNTER(ConnectorConnectFail);
SERVER_COUNTER(ConnectorConnectSuccess);
SERVER_COUNTER(ConnectorDoSocketRead);
SERVER_COUNTER(ConnectorFinishRun);
SERVER_COUNTER(ConnectorFinishWaitShutdownAck);
SERVER_COUNTER(ConnectorSocketBrokerClose);
SERVER_COUNTER(ConnectorSocketError);
SERVER_COUNTER(ConnectorSocketReadSuccess);
SERVER_COUNTER(ConnectorSocketTimeout);
SERVER_COUNTER(ConnectorStartConnect);
SERVER_COUNTER(ConnectorStartFastShutdown);
SERVER_COUNTER(ConnectorStartRun);
SERVER_COUNTER(ConnectorStartSlowShutdown);
SERVER_COUNTER(ConnectorStartWaitShutdownAck);
SERVER_COUNTER(ConnectorTruncateLongTimeout);
SERVER_COUNTER(SendProduceRequestOk);

TConnector::TConnector(size_t my_broker_index, TDispatcherSharedState &ds)
    : MyBrokerIndex(my_broker_index),
      Ds(ds),
      DebugLoggerSend(ds.DebugSetup, TDebugSetup::TLogId::MSG_SEND),
      DebugLoggerReceive(ds.DebugSetup, TDebugSetup::TLogId::MSG_GOT_ACK),
      InputQueue(ds.BatchConfig, ds.MsgStateTracker),
      /* TODO: rethink DebugLogger stuff */
      RequestFactory(ds.Config, ds.BatchConfig, ds.CompressionConf,
                     ds.ProduceProtocol, my_broker_index),
      PauseInProgress(false),
      Destroying(false),
      ResponseReader(ds.ProduceProtocol->CreateProduceResponseReader()),
      /* Note: The max message body size value is a loose upper bound to guard
         against a response with a ridiculously large size field. */
      StreamReader(false, true, 4 * 1024 * 1024, 64 * 1024),
      OkShutdown(true) {
  static_assert(sizeof(TStreamReader::TSizeFieldType) ==
      REQUEST_OR_RESPONSE_SIZE_SIZE, "Wrong size field size for StreamReader");
}

TConnector::~TConnector() noexcept {
  /* This will shut down the thread if something unexpected happens.  Setting
     the 'Destroying' flag tells the thread to shut down immediately when it
     gets the shutdown request. */
  Destroying = true;
  ShutdownOnDestroy();
}

void TConnector::SetMetadata(const std::shared_ptr<TMetadata> &md) {
  assert(this);
  assert(md);
  Metadata = md;
  RequestFactory.Init(Ds.CompressionConf, md);
}

void TConnector::StartSlowShutdown(uint64_t start_time) {
  assert(this);
  assert(IsStarted());
  assert(!OptShutdownCmd.IsKnown());
  ConnectorStartSlowShutdown.Increment();
  syslog(LOG_NOTICE, "Sending slow shutdown request to connector thread "
       "(index %lu broker %ld)", static_cast<unsigned long>(MyBrokerIndex),
       MyBrokerId());
  OptShutdownCmd.MakeKnown(start_time);
  RequestShutdown();
}

void TConnector::StartFastShutdown() {
  assert(this);
  assert(IsStarted());
  assert(!OptShutdownCmd.IsKnown());
  ConnectorStartFastShutdown.Increment();
  syslog(LOG_NOTICE, "Sending fast shutdown request to connector thread "
      "(index %lu broker %ld)", static_cast<unsigned long>(MyBrokerIndex),
      MyBrokerId());
  OptShutdownCmd.MakeKnown();
  RequestShutdown();
}

void TConnector::WaitForShutdownAck() {
  assert(this);
  ConnectorStartWaitShutdownAck.Increment();
  long broker_id = MyBrokerId();
  syslog(LOG_NOTICE, "Waiting for shutdown ACK from connector thread (index "
      "%lu broker %ld)", static_cast<unsigned long>(MyBrokerIndex), broker_id);

  /* In addition to waiting for the shutdown ACK, we must wait for shutdown
     finished, since the thread may have started shutting down on its own
     immediately before we sent the shutdown request. */
  static const size_t POLL_ARRAY_SIZE = 2;
  struct pollfd poll_array[POLL_ARRAY_SIZE];
  poll_array[0].fd = ShutdownAck.GetFd();
  poll_array[0].events = POLLIN;
  poll_array[0].revents = 0;
  poll_array[1].fd = GetShutdownWaitFd();
  poll_array[1].events = POLLIN;
  poll_array[1].revents = 0;

  /* Don't check for EINTR, since this thread has signals masked. */
  IfLt0(poll(poll_array, POLL_ARRAY_SIZE, -1));

  const char *blurb = poll_array[0].revents ?
      "shutdown ACK" : "shutdown finished notification";
  syslog(LOG_NOTICE, "Got %s from connector thread (index %lu broker %ld)",
      blurb, static_cast<unsigned long>(MyBrokerIndex), broker_id);
  ConnectorFinishWaitShutdownAck.Increment();
  OptShutdownCmd.Reset();
}

void TConnector::CleanupAfterJoin() {
  assert(this);
  assert(SendWaitAfterShutdown.empty());
  assert(NoAckAfterShutdown.empty());
  assert(!Destroying);
  ConnectorCleanupAfterJoin.Increment();
  Metadata.reset();

  /* The order of the remaining steps matters because we want to avoid getting
     messages unnecessarily out of order. */

  if (CurrentRequest.IsKnown()) {
    EmptyAllTopics(CurrentRequest->second, SendWaitAfterShutdown);
  }

  SendWaitAfterShutdown.splice(SendWaitAfterShutdown.end(),
      std::move(GotAckAfterPause));
  SendWaitAfterShutdown.splice(SendWaitAfterShutdown.end(),
      RequestFactory.GetAll());
  SendWaitAfterShutdown.splice(SendWaitAfterShutdown.end(),
      InputQueue.Reset());
  NoAckAfterShutdown.splice(NoAckAfterShutdown.end(),
      std::move(NoAckAfterPause));

  for (TProduceRequest &request : AckWaitQueue) {
    EmptyAllTopics(request.second, NoAckAfterShutdown);
  }

  /* After emptying out the connector, don't bother reinitializing it to a
     newly constructed state.  It will be destroyed and recreated before the
     dispatcher restarts. */
}

void TConnector::Run() {
  assert(this);
  assert(Metadata);
  ConnectorStartRun.Increment();
  long broker_id = ~0;

  try {
    TOnDestroy close_socket(
        [this]() noexcept {
          Sock.Reset();  // close TCP connection to broker if open
        }
    );
    assert(MyBrokerIndex < Metadata->GetBrokers().size());
    broker_id = MyBrokerId();
    syslog(LOG_NOTICE, "Connector thread %d (index %lu broker %ld) started",
        static_cast<int>(Gettid()), static_cast<unsigned long>(MyBrokerIndex),
        broker_id);
    DoRun();
  } catch (const TShutdownOnDestroy &) {
    /* Nothing to do here. */
  } catch (const std::exception &x) {
    syslog(LOG_ERR, "Fatal error in connector thread %d (index %lu broker "
        "%ld): %s", static_cast<int>(Gettid()),
        static_cast<unsigned long>(MyBrokerIndex), broker_id, x.what());
    _exit(EXIT_FAILURE);
  } catch (...) {
    syslog(LOG_ERR, "Fatal unknown error in connector thread %d (index %lu "
        "broker %ld)", static_cast<int>(Gettid()),
        static_cast<unsigned long>(MyBrokerIndex), broker_id);
    _exit(EXIT_FAILURE);
  }

  syslog(LOG_NOTICE, "Connector thread %d (index %lu broker %ld) finished %s",
      static_cast<int>(Gettid()), static_cast<unsigned long>(MyBrokerIndex),
      broker_id, OkShutdown ? "normally" : "on error");
  Ds.MarkThreadFinished();
  ConnectorFinishRun.Increment();
}

bool TConnector::DoConnect() {
  assert(!Sock.IsOpen());
  const TMetadata::TBroker &broker = MyBroker();
  assert(broker.IsInService());
  const std::string &host = broker.GetHostname();
  uint16_t port = broker.GetPort();
  long broker_id = broker.GetId();
  syslog(LOG_NOTICE, "Connector thread %d (index %lu broker %ld) connecting "
      "to host %s port %u", static_cast<int>(Gettid()),
      static_cast<unsigned long>(MyBrokerIndex), broker_id, host.c_str(),
      static_cast<unsigned>(port));

  try {
    ConnectToHost(host, port, Sock);
  } catch (const std::system_error &x) {
    syslog(LOG_ERR, "Starting pause on failure to connect to broker %s port "
        "%u: %s", host.c_str(), static_cast<unsigned>(port), x.what());
    assert(!Sock.IsOpen());
    return false;
  } catch (const Socket::Db::TError &x) {
    syslog(LOG_ERR, "Starting pause on failure to connect to broker %s port "
        "%u: %s", host.c_str(), static_cast<unsigned>(port), x.what());
    assert(!Sock.IsOpen());
    return false;
  }

  if (!Sock.IsOpen()) {
    syslog(LOG_ERR, "Starting pause on failure to connect to broker %s port "
        "%u", host.c_str(), static_cast<unsigned>(port));
    return false;
  }

  syslog(LOG_NOTICE, "Connector thread %d (index %lu broker %ld) connect "
      "successful", static_cast<int>(Gettid()),
      static_cast<unsigned long>(MyBrokerIndex), broker_id);
  return true;
}

bool TConnector::ConnectToBroker() {
  assert(this);
  ConnectorStartConnect.Increment();
  bool success = DoConnect();

  if (success) {
    ConnectorConnectSuccess.Increment();
  } else {
    ConnectorConnectFail.Increment();
    Ds.PauseButton.Push();
  }

  return success;
}

static int AdjustTimeoutByDeadline(int initial_timeout, uint64_t now,
    uint64_t deadline, const char *error_blurb) {
  uint64_t full_deadline_timeout = (now > deadline) ? 0 : (deadline - now);
  int deadline_timeout = static_cast<int>(full_deadline_timeout);

  if (full_deadline_timeout >
      static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    deadline_timeout = std::numeric_limits<int>::max();
    syslog(LOG_WARNING, "Truncating ridiculously long %s timeout %lu in "
        "connector thread", error_blurb,
        static_cast<unsigned long>(full_deadline_timeout));
    ConnectorTruncateLongTimeout.Increment();
  }

  return (initial_timeout < 0) ?
      deadline_timeout : std::min(initial_timeout, deadline_timeout);
}

void TConnector::SetFastShutdownState() {
  assert(this);
  uint64_t deadline = GetEpochMilliseconds() +
      Ds.Config.DispatcherRestartMaxDelay;

  if (OptInProgressShutdown.IsKnown()) {
    TInProgressShutdown &shutdown_state = *OptInProgressShutdown;
    shutdown_state.Deadline = std::min(shutdown_state.Deadline, deadline);
    shutdown_state.FastShutdown = true;
  } else {
    OptInProgressShutdown.MakeKnown(deadline, true);
  }
}

void TConnector::HandleShutdownRequest() {
  assert(this);

  if (Destroying) {
    throw TShutdownOnDestroy();
  }

  assert(OptShutdownCmd.IsKnown());
  const TShutdownCmd &cmd = *OptShutdownCmd;
  bool is_fast = cmd.OptSlowShutdownStartTime.IsUnknown();

  if (is_fast) {
    SetFastShutdownState();
  } else {
    /* Before sending the slow shutdown request, the router thread routed all
       remaining messages to the dispatcher.  Get all remaining messages before
       we stop monitoring our input queue. */
    RequestFactory.Put(InputQueue.GetAllOnShutdown());

    uint64_t deadline = *cmd.OptSlowShutdownStartTime +
        Ds.Config.ShutdownMaxDelay;

    if (OptInProgressShutdown.IsKnown()) {
      TInProgressShutdown &shutdown_state = *OptInProgressShutdown;
      shutdown_state.Deadline = std::min(shutdown_state.Deadline, deadline);
    } else {
      OptInProgressShutdown.MakeKnown(deadline, false);
    }
  }

  syslog(LOG_NOTICE, "Connector thread %d (index %lu broker %ld) sending ACK "
      "for %s shutdown", static_cast<int>(Gettid()),
      static_cast<unsigned long>(MyBrokerIndex), MyBrokerId(),
      is_fast ? "fast" : "slow");
  ShutdownAck.Push();
  ClearShutdownRequest();
}

void TConnector::SetPauseInProgress() {
  assert(this);
  PauseInProgress = true;
  SetFastShutdownState();
}

void TConnector::HandlePauseDetected() {
  assert(this);
  syslog(LOG_NOTICE, "Connector thread %d (index %lu broker %ld) detected "
      "pause: starting fast shutdown", static_cast<int>(Gettid()),
      static_cast<unsigned long>(MyBrokerIndex), MyBrokerId());
  SetPauseInProgress();
}

void TConnector::CheckInputQueue(uint64_t now, bool pop_sem) {
  assert(this);
  ConnectorCheckInputQueue.Increment();
  std::list<std::list<TMsg::TPtr>> ready_msgs;
  TMsg::TTimestamp expiry = 0;
  bool has_expiry = pop_sem ?
      InputQueue.Get(now, expiry, ready_msgs) :
      InputQueue.NonblockingGet(now, expiry, ready_msgs);
  OptNextBatchExpiry.Reset();

  if (has_expiry) {
    OptNextBatchExpiry.MakeKnown(expiry);
  }

  RequestFactory.Put(std::move(ready_msgs));
}

bool TConnector::TrySendProduceRequest() {
  assert(this);

  try {
    SendBuf.MarkDataConsumed(IfLt0(
        send(Sock, SendBuf.Data(), SendBuf.DataSize(), MSG_NOSIGNAL)));
  } catch (const std::system_error &x) {
    if (LostTcpConnection(x)) {
      syslog(LOG_ERR, "Connector thread %d (index %lu broker %ld) starting "
          "pause and finishing due to lost TCP connection during send: %s",
          static_cast<int>(Gettid()),
          static_cast<unsigned long>(MyBrokerIndex), MyBrokerId(), x.what());
      ConnectorSocketError.Increment();
      Ds.PauseButton.Push();
      return false;
    }

    throw;  // anything else is fatal
  }

  /* Data was sent successfully, although maybe not as much as requested.  If
     any unsent data remains, we will continue sending when the socket becomes
     ready again for writing. */
  return true;
}

bool TConnector::HandleSockWriteReady() {
  assert(this);
  assert(CurrentRequest.IsKnown() == SendInProgress());

  /* See whether we are starting a new produce request, or continuing a
     partially sent one. */
  if (!SendInProgress()) {
    std::vector<uint8_t> buf(SendBuf.TakeStorage());
    CurrentRequest = RequestFactory.BuildRequest(buf);

    if (CurrentRequest.IsUnknown()) {
      assert(false);
      syslog(LOG_ERR, "Bug!!! Produce request is empty");
      BugProduceRequestEmpty.Increment();
      return true;
    }

    SendBuf = std::move(buf);
    assert(!SendBuf.DataIsEmpty());
  }

  if (!TrySendProduceRequest()) {
    /* Socket error on attempted send: pause has been initiated.  Leave
       'CurrentRequest' in place, and the messages it contains will be
       rerouted once we have new metadata and the dispatcher has been
       restarted. */
    return false;
  }

  if (!SendInProgress()) {
    /* We finished sending the request.  Now expect a response from Kafka,
       unless RequiredAcks is 0. */

    SendProduceRequestOk.Increment();
    TAllTopics &all_topics = CurrentRequest->second;
    bool ack_expected = (Ds.Config.RequiredAcks != 0);

    for (auto &topic_elem : all_topics) {
      TMultiPartitionGroup &group = topic_elem.second;

      for (auto &msg_set_elem : group) {
        if (ack_expected) {
          Ds.MsgStateTracker.MsgEnterAckWait(msg_set_elem.second.Contents);
        } else {
          AckNotRequired.Increment();
          Ds.MsgStateTracker.MsgEnterProcessed(msg_set_elem.second.Contents);
        }

        DebugLoggerSend.LogMsgList(msg_set_elem.second.Contents);
      }
    }

    if (ack_expected) {
      AckWaitQueue.emplace_back(std::move(*CurrentRequest));
    }

    CurrentRequest.Reset();
  }

  return true;
}

bool TConnector::ProcessSingleProduceResponse() {
  assert(this);
  assert(!AckWaitQueue.empty());
  assert(StreamReader.GetState() == TStreamMsgReader::TState::MsgReady);
  bool keep_running = true;
  bool pause = false;
  TProduceRequest request(std::move(AckWaitQueue.front()));
  AckWaitQueue.pop_front();
  TProduceResponseProcessor processor(*ResponseReader, Ds, DebugLoggerReceive,
      MyBrokerIndex, MyBrokerId());

  try {
    switch (processor.ProcessResponse(request, StreamReader.GetReadyMsg(),
        StreamReader.GetReadyMsgSize())) {
      case TProduceResponseProcessor::TAction::KeepRunning: {
        break;
      }
      case TProduceResponseProcessor::TAction::PauseAndDeferFinish: {
        /* Start pause but keep processing produce responses until fast
           shutdown time limit expiry. */
        SetPauseInProgress();
        pause = true;
        break;
      }
      case TProduceResponseProcessor::TAction::PauseAndFinishNow: {
        /* A serious enough error occurred that communication with the broker
           can not continue.  Shut down immediately after telling the other
           threads to pause. */
        keep_running = false;
        pause = true;

        /* Handle any messages that we got no ACK for. */
        NoAckAfterPause.splice(NoAckAfterPause.end(),
            processor.TakeMsgsWithoutAcks());
        break;
      }
      NO_DEFAULT_CASE;
    }
  } catch (const TProduceResponseReaderApi::TBadProduceResponse &x) {
    syslog(LOG_ERR, "Connector thread %d (index %lu broker %ld) starting "
        "pause due to unexpected response from broker: %s",
        static_cast<int>(Gettid()), static_cast<unsigned long>(MyBrokerIndex),
        MyBrokerId(), x.what());
    BadProduceResponse.Increment();
    keep_running = false;
    pause = true;
  }

  if (pause) {
    Ds.PauseButton.Push();

    /* Handle any messages for which we got an error ACK that requires
       rerouting based on new metadata. */
    GotAckAfterPause.splice(GotAckAfterPause.end(),
        processor.TakePauseAndResendAckMsgs());
  }

  /* Handle any messages that got error ACKs allowing immediate retransmission
     without rerouting based on new metadata. */
  RequestFactory.PutFront(processor.TakeImmediateResendAckMsgs());

  return keep_running;
}

/* Attempt a single large read (possibly more bytes than a single produce
   response will require).  Then consider the following cases:

       Case 1: We got a socket error.  Return false to notify the main loop
           that an error occurred.

       Case 2: While processing the response data, at some point we either
           found something invalid in the response or got an error ACK
           indicating the need for new metadata.  In this case, return false to
           notify the main loop of the error.  If a response was partially
           processed when the error was detected, we will leave behind enough
           state that things can be sorted out once the dispatcher has finished
           shutting down in preparation for the metadata update.

       Case 3: We got some data that looks valid at first glance, but there is
           not enough to complete a produce response.  Leave the data we got in
           the buffer and return true (indicating no error).  The main loop
           will call us again when it detects that the socket is ready.

       Case 4: We got enough data to complete at least one produce response,
           and encountered no serious errors while processing it.  In this
           case, we process the data in the buffer (possibly multiple produce
           responses) until there is not enough left for another complete
           produce response.  Then return true to indicate no error.  The main
           loop will call us again when appropriate. */
bool TConnector::HandleSockReadReady() {
  assert(this);
  assert(!AckWaitQueue.empty());
  TStreamMsgReader::TState reader_state = TStreamMsgReader::TState::AtEnd;

  try {
    reader_state = StreamReader.Read();
  } catch (const std::system_error &x) {
    if (LostTcpConnection(x)) {
      syslog(LOG_ERR, "Connector thread %d (index %lu broker %ld) starting "
          "pause due to lost TCP connection on attempted read: %s",
          static_cast<int>(Gettid()),
          static_cast<unsigned long>(MyBrokerIndex), MyBrokerId(), x.what());
      ConnectorSocketError.Increment();
      Ds.PauseButton.Push();
      return false;
    }

    throw;  // anything else is fatal
  }

  for (; ; ) {
    switch (reader_state) {
      case TStreamMsgReader::TState::ReadNeeded: {
        return true;
      }
      case TStreamMsgReader::TState::MsgReady: {
        break;
      }
      case TStreamMsgReader::TState::DataInvalid: {
        syslog(LOG_ERR, "Connector thread %d (index %lu broker %ld) starting "
            "pause due to invalid response size response from broker",
            static_cast<int>(Gettid()),
            static_cast<unsigned long>(MyBrokerIndex), MyBrokerId());
        BadProduceResponseSize.Increment();
        Ds.PauseButton.Push();
        return false;
      }
      case TStreamMsgReader::TState::AtEnd: {
        syslog(LOG_ERR, "Connector thread %d (index %lu broker %ld) starting "
            "pause because TCP connection unexpectedly closed by broker while "
            "processing produce responses", static_cast<int>(Gettid()),
            static_cast<unsigned long>(MyBrokerIndex), MyBrokerId());
        ConnectorSocketBrokerClose.Increment();
        Ds.PauseButton.Push();
        return false;
      }
      NO_DEFAULT_CASE;
    }

    if (!ProcessSingleProduceResponse()) {
      break;  // error processing produce response
    }

    /* Mark produce response as consumed. */
    reader_state = StreamReader.ConsumeReadyMsg();

    if (AckWaitQueue.empty() &&
        (reader_state == TStreamMsgReader::TState::MsgReady)) {
      syslog(LOG_ERR, "Connector thread %d (index %lu broker %ld) starting "
          "pause due to unexpected response data from broker during response "
          "processing", static_cast<int>(Gettid()),
          static_cast<unsigned long>(MyBrokerIndex), MyBrokerId());
      Ds.PauseButton.Push();
      break;
    }
  }

  return false;  // we only get here on error
}

bool TConnector::PrepareForPoll(uint64_t now, int &poll_timeout) {
  assert(this);
  poll_timeout = -1;
  bool need_sock_write = false;
  bool need_sock_read = !AckWaitQueue.empty();
  bool need_shutdown_timeout = false;
  bool need_batch_timeout = false;

  /* When we set 'PauseInProgress', we also activate fast shutdown.  Therefore
     the logic below prevents us from starting a new send or monitoring for
     batch expiry once we have detected a pause event. */
  assert(!PauseInProgress ||
      (OptInProgressShutdown.IsKnown() &&
          OptInProgressShutdown->FastShutdown));

  if (SendInProgress()) {
    need_sock_write = true;

    /* We have a partially sent produce request.  In this case, finish sending
       the request even if the shutdown timeout is exceeded.  Until the send is
       finished, we don't need to monitor for batch expiry since batched
       messages can't be sent until the current send finishes. */
  } else if (OptInProgressShutdown.IsKnown()) {
    /* A fast or slow shutdown is in progress.  In the case of a fast shutdown,
       stop sending immediately since no partially sent request needs
       finishing.  In the case of a slow shutdown, keep sending until there is
       nothing more to send or the time limit expires. */
    need_sock_write = !RequestFactory.IsEmpty() &&
        !OptInProgressShutdown->FastShutdown;

    if (!need_sock_write && !need_sock_read) {
      /* We have no more requests to send or responses to receive, so shut down
         immediately. */
      return false;
    }

    need_shutdown_timeout = true;

    /* If a fast shutdown is in progress, we are done sending so we no longer
       need to monitor for batch expiry. */
    need_batch_timeout = OptNextBatchExpiry.IsKnown() &&
        !OptInProgressShutdown->FastShutdown;
  } else {
    need_sock_write = !RequestFactory.IsEmpty();
    need_batch_timeout = OptNextBatchExpiry.IsKnown();
  }

  if (need_sock_write || need_sock_read) {
    poll_timeout = static_cast<int>(Ds.Config.KafkaSocketTimeout * 1000);
  }

  if (need_shutdown_timeout) {
    poll_timeout = AdjustTimeoutByDeadline(poll_timeout, now,
        OptInProgressShutdown->Deadline, "shutdown");
  }

  if (need_batch_timeout) {
    poll_timeout = AdjustTimeoutByDeadline(poll_timeout, now,
        *OptNextBatchExpiry, "batch");
  }

  struct pollfd &sock_item = MainLoopPollArray[TMainLoopPollItem::SockIo];
  struct pollfd &shutdown_item =
      MainLoopPollArray[TMainLoopPollItem::ShutdownRequest];
  struct pollfd &pause_item =
      MainLoopPollArray[TMainLoopPollItem::PauseButton];
  struct pollfd &input_item = MainLoopPollArray[TMainLoopPollItem::InputQueue];

  sock_item.events = 0;
  sock_item.revents = 0;

  if (need_sock_write) {
    sock_item.events |= POLLOUT;
  }

  if (need_sock_read) {
    sock_item.events |= POLLIN;
  }

  sock_item.fd = sock_item.events ? int(Sock) : -1;
  shutdown_item.fd = GetShutdownRequestFd();
  shutdown_item.events = POLLIN;
  shutdown_item.revents = 0;
  pause_item.fd = PauseInProgress ? -1 : int(Ds.PauseButton.GetFd());
  pause_item.events = POLLIN;
  pause_item.revents = 0;

  /* Stop monitoring the input queue when a fast or slow shutdown is in
     progress.  In the case of a slow shutdown, we have already emptied it
     and know that no more requests  will be queued.  Note that
     'PauseInProgress' implies fast shutdown. */
  input_item.fd = OptInProgressShutdown.IsKnown() ?
      -1 : int(InputQueue.GetSenderNotifyFd());

  input_item.events = POLLIN;
  input_item.revents = 0;
  return true;
}

void TConnector::DoRun() {
  assert(this);
  OkShutdown = false;
  long broker_id = MyBrokerId();

  if (!ConnectToBroker()) {
    return;
  }

  StreamReader.Reset(Sock);

  for (; ; ) {
    int poll_timeout = -1;
    uint64_t start_time = GetEpochMilliseconds();

    if (!PrepareForPoll(start_time, poll_timeout)) {
      OkShutdown = true;
      break;
    }

    /* Don't check for EINTR, since this thread has signals masked. */
    int ret = IfLt0(poll(MainLoopPollArray, MainLoopPollArray.Size(),
        poll_timeout));

    /* Handle possibly nonmonotonic clock.
       TODO: Use monotonic clock instead. */
    uint64_t finish_time = std::max(start_time, GetEpochMilliseconds());

    if (ret == 0) {  // poll() timed out
      if ((MainLoopPollArray[TMainLoopPollItem::SockIo].fd >= 0) &&
          ((finish_time - start_time) >=
              (Ds.Config.KafkaSocketTimeout * 1000))) {
        syslog(LOG_ERR, "Connector thread %d (index %lu broker %ld) starting "
            "pause due to socket timeout in main loop",
            static_cast<int>(Gettid()),
            static_cast<unsigned long>(MyBrokerIndex), broker_id);
        ConnectorSocketTimeout.Increment();
        Ds.PauseButton.Push();
        break;
      }

      if (OptInProgressShutdown.IsKnown() &&
          (finish_time >= OptInProgressShutdown->Deadline)) {
        OkShutdown = true;
        syslog(LOG_NOTICE, "Connector thread %d (index %lu broker %ld) "
            "finishing on shutdown time limit expiration",
            static_cast<int>(Gettid()),
            static_cast<unsigned long>(MyBrokerIndex), broker_id);
        break;
      }

      /* Handle batch time limit expiry. */
      CheckInputQueue(finish_time, false);
    } else if (MainLoopPollArray[TMainLoopPollItem::ShutdownRequest].revents) {
      /* Give this FD the highest priority since we must shut down immediately
         if 'Destroying' is set. */
      HandleShutdownRequest();
      /* Handle other FDs in next iteration. */
    } else if (MainLoopPollArray[TMainLoopPollItem::PauseButton].revents) {
      HandlePauseDetected();
      /* Handle other FDs in next iteration. */
    } else {
      if (MainLoopPollArray[TMainLoopPollItem::InputQueue].revents) {
        CheckInputQueue(finish_time, true);
      }

      short sock_events = MainLoopPollArray[TMainLoopPollItem::SockIo].revents;

      if ((sock_events & POLLOUT) && !HandleSockWriteReady()) {
        break;  // socket error on send
      }

      if ((sock_events & POLLIN) && !HandleSockReadReady()) {
        break;
      }
    }
  }
}
