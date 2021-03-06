// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/message_pipe.h"

#include "base/logging.h"
#include "mojo/edk/system/channel.h"
#include "mojo/edk/system/channel_endpoint.h"
#include "mojo/edk/system/channel_endpoint_id.h"
#include "mojo/edk/system/local_message_pipe_endpoint.h"
#include "mojo/edk/system/message_in_transit.h"
#include "mojo/edk/system/message_pipe_dispatcher.h"
#include "mojo/edk/system/message_pipe_endpoint.h"
#include "mojo/edk/system/proxy_message_pipe_endpoint.h"

namespace mojo {
namespace system {

namespace {

// TODO(vtl): Move this into |Channel| (and possible further).
struct SerializedMessagePipe {
  // This is the endpoint ID on the receiving side, and should be a "remote ID".
  // (The receiving side should already have had an endpoint attached and been
  // run via the |Channel|s. This endpoint will have both IDs assigned, so this
  // ID is only needed to associate that endpoint with a particular dispatcher.)
  ChannelEndpointId receiver_endpoint_id;
};

}  // namespace

// static
MessagePipe* MessagePipe::CreateLocalLocal() {
  MessagePipe* message_pipe = new MessagePipe();
  message_pipe->endpoints_[0].reset(new LocalMessagePipeEndpoint());
  message_pipe->endpoints_[1].reset(new LocalMessagePipeEndpoint());
  return message_pipe;
}

// static
MessagePipe* MessagePipe::CreateLocalProxy(
    scoped_refptr<ChannelEndpoint>* channel_endpoint) {
  DCHECK(!channel_endpoint->get());  // Not technically wrong, but unlikely.
  MessagePipe* message_pipe = new MessagePipe();
  message_pipe->endpoints_[0].reset(new LocalMessagePipeEndpoint());
  *channel_endpoint = new ChannelEndpoint(message_pipe, 1);
  message_pipe->endpoints_[1].reset(
      new ProxyMessagePipeEndpoint(channel_endpoint->get()));
  return message_pipe;
}

// static
MessagePipe* MessagePipe::CreateProxyLocal(
    scoped_refptr<ChannelEndpoint>* channel_endpoint) {
  DCHECK(!channel_endpoint->get());  // Not technically wrong, but unlikely.
  MessagePipe* message_pipe = new MessagePipe();
  *channel_endpoint = new ChannelEndpoint(message_pipe, 0);
  message_pipe->endpoints_[0].reset(
      new ProxyMessagePipeEndpoint(channel_endpoint->get()));
  message_pipe->endpoints_[1].reset(new LocalMessagePipeEndpoint());
  return message_pipe;
}

// static
unsigned MessagePipe::GetPeerPort(unsigned port) {
  DCHECK(port == 0 || port == 1);
  return port ^ 1;
}

// static
bool MessagePipe::Deserialize(Channel* channel,
                              const void* source,
                              size_t size,
                              scoped_refptr<MessagePipe>* message_pipe,
                              unsigned* port) {
  DCHECK(!message_pipe->get());  // Not technically wrong, but unlikely.

  if (size != sizeof(SerializedMessagePipe)) {
    LOG(ERROR) << "Invalid serialized message pipe";
    return false;
  }

  const SerializedMessagePipe* s =
      static_cast<const SerializedMessagePipe*>(source);
  *message_pipe = channel->PassIncomingMessagePipe(s->receiver_endpoint_id);
  if (!message_pipe->get()) {
    LOG(ERROR) << "Failed to deserialize message pipe (ID = "
               << s->receiver_endpoint_id << ")";
    return false;
  }

  DVLOG(2) << "Deserializing message pipe dispatcher (new local ID = "
           << s->receiver_endpoint_id << ")";
  *port = 0;
  return true;
}

MessagePipeEndpoint::Type MessagePipe::GetType(unsigned port) {
  DCHECK(port == 0 || port == 1);
  base::AutoLock locker(lock_);
  DCHECK(endpoints_[port]);

  return endpoints_[port]->GetType();
}

void MessagePipe::CancelAllWaiters(unsigned port) {
  DCHECK(port == 0 || port == 1);

  base::AutoLock locker(lock_);
  DCHECK(endpoints_[port]);
  endpoints_[port]->CancelAllWaiters();
}

void MessagePipe::Close(unsigned port) {
  DCHECK(port == 0 || port == 1);

  unsigned destination_port = GetPeerPort(port);

  base::AutoLock locker(lock_);
  // The endpoint's |OnPeerClose()| may have been called first and returned
  // false, which would have resulted in its destruction.
  if (!endpoints_[port])
    return;

  endpoints_[port]->Close();
  if (endpoints_[destination_port]) {
    if (!endpoints_[destination_port]->OnPeerClose())
      endpoints_[destination_port].reset();
  }
  endpoints_[port].reset();
}

// TODO(vtl): Handle flags.
MojoResult MessagePipe::WriteMessage(
    unsigned port,
    UserPointer<const void> bytes,
    uint32_t num_bytes,
    std::vector<DispatcherTransport>* transports,
    MojoWriteMessageFlags flags) {
  DCHECK(port == 0 || port == 1);
  return EnqueueMessage(
      GetPeerPort(port),
      make_scoped_ptr(new MessageInTransit(
          MessageInTransit::kTypeMessagePipeEndpoint,
          MessageInTransit::kSubtypeMessagePipeEndpointData, num_bytes, bytes)),
      transports);
}

MojoResult MessagePipe::ReadMessage(unsigned port,
                                    UserPointer<void> bytes,
                                    UserPointer<uint32_t> num_bytes,
                                    DispatcherVector* dispatchers,
                                    uint32_t* num_dispatchers,
                                    MojoReadMessageFlags flags) {
  DCHECK(port == 0 || port == 1);

  base::AutoLock locker(lock_);
  DCHECK(endpoints_[port]);

  return endpoints_[port]->ReadMessage(bytes, num_bytes, dispatchers,
                                       num_dispatchers, flags);
}

HandleSignalsState MessagePipe::GetHandleSignalsState(unsigned port) const {
  DCHECK(port == 0 || port == 1);

  base::AutoLock locker(const_cast<base::Lock&>(lock_));
  DCHECK(endpoints_[port]);

  return endpoints_[port]->GetHandleSignalsState();
}

MojoResult MessagePipe::AddWaiter(unsigned port,
                                  Waiter* waiter,
                                  MojoHandleSignals signals,
                                  uint32_t context,
                                  HandleSignalsState* signals_state) {
  DCHECK(port == 0 || port == 1);

  base::AutoLock locker(lock_);
  DCHECK(endpoints_[port]);

  return endpoints_[port]->AddWaiter(waiter, signals, context, signals_state);
}

void MessagePipe::RemoveWaiter(unsigned port,
                               Waiter* waiter,
                               HandleSignalsState* signals_state) {
  DCHECK(port == 0 || port == 1);

  base::AutoLock locker(lock_);
  DCHECK(endpoints_[port]);

  endpoints_[port]->RemoveWaiter(waiter, signals_state);
}

void MessagePipe::StartSerialize(unsigned /*port*/,
                                 Channel* /*channel*/,
                                 size_t* max_size,
                                 size_t* max_platform_handles) {
  *max_size = sizeof(SerializedMessagePipe);
  *max_platform_handles = 0;
}

bool MessagePipe::EndSerialize(
    unsigned port,
    Channel* channel,
    void* destination,
    size_t* actual_size,
    embedder::PlatformHandleVector* /*platform_handles*/) {
  DCHECK(port == 0 || port == 1);

  scoped_refptr<ChannelEndpoint> channel_endpoint;
  {
    base::AutoLock locker(lock_);
    DCHECK(endpoints_[port]);

    // The port being serialized must be local.
    DCHECK_EQ(endpoints_[port]->GetType(), MessagePipeEndpoint::kTypeLocal);

    // There are three possibilities for the peer port (below). In all cases, we
    // pass the contents of |port|'s message queue to the channel, and it'll
    // (presumably) make a |ChannelEndpoint| from it.
    //
    //  1. The peer port is (known to be) closed.
    //
    //     There's no reason for us to continue to exist and no need for the
    //     channel to give us the |ChannelEndpoint|. It only remains for us to
    //     "close" |port|'s |LocalMessagePipeEndpoint| and prepare for
    //     destruction.
    //
    //  2. The peer port is local (the typical case).
    //
    //     The channel gives us back a |ChannelEndpoint|, which we hook up to a
    //     |ProxyMessagePipeEndpoint| to replace |port|'s
    //     |LocalMessagePipeEndpoint|. We continue to exist, since the peer
    //     port's message pipe dispatcher will continue to hold a reference to
    //     us.
    //
    //  3. The peer port is remote.
    //
    //     We also pass its |ChannelEndpoint| to the channel, which then decides
    //     what to do. We have no reason to continue to exist.
    //
    // TODO(vtl): Factor some of this out to |ChannelEndpoint|.

    if (!endpoints_[GetPeerPort(port)]) {
      // Case 1.
      channel_endpoint = new ChannelEndpoint(
          nullptr, 0, static_cast<LocalMessagePipeEndpoint*>(
                          endpoints_[port].get())->message_queue());
      endpoints_[port]->Close();
      endpoints_[port].reset();
    } else if (endpoints_[GetPeerPort(port)]->GetType() ==
               MessagePipeEndpoint::kTypeLocal) {
      // Case 2.
      channel_endpoint = new ChannelEndpoint(
          this, port, static_cast<LocalMessagePipeEndpoint*>(
                          endpoints_[port].get())->message_queue());
      endpoints_[port]->Close();
      endpoints_[port].reset(
          new ProxyMessagePipeEndpoint(channel_endpoint.get()));
    } else {
      // Case 3.
      // TODO(vtl): Temporarily the same as case 2.
      DLOG(WARNING) << "Direct message pipe passing across multiple channels "
                       "not yet implemented; will proxy";
      channel_endpoint = new ChannelEndpoint(
          this, port, static_cast<LocalMessagePipeEndpoint*>(
                          endpoints_[port].get())->message_queue());
      endpoints_[port]->Close();
      endpoints_[port].reset(
          new ProxyMessagePipeEndpoint(channel_endpoint.get()));
    }
  }

  SerializedMessagePipe* s = static_cast<SerializedMessagePipe*>(destination);

  // Convert the local endpoint to a proxy endpoint (moving the message queue)
  // and attach it to the channel.
  s->receiver_endpoint_id =
      channel->AttachAndRunEndpoint(channel_endpoint, false);
  DVLOG(2) << "Serializing message pipe (remote ID = "
           << s->receiver_endpoint_id << ")";
  *actual_size = sizeof(SerializedMessagePipe);
  return true;
}

bool MessagePipe::OnReadMessage(unsigned port,
                                scoped_ptr<MessageInTransit> message) {
  // This is called when the |ChannelEndpoint| for the
  // |ProxyMessagePipeEndpoint| |port| receives a message (from the |Channel|).
  // We need to pass this message on to its peer port (typically a
  // |LocalMessagePipeEndpoint|).
  return EnqueueMessage(GetPeerPort(port), message.Pass(), nullptr) ==
         MOJO_RESULT_OK;
}

void MessagePipe::OnDetachFromChannel(unsigned port) {
  Close(port);
}

MessagePipe::MessagePipe() {
}

MessagePipe::~MessagePipe() {
  // Owned by the dispatchers. The owning dispatchers should only release us via
  // their |Close()| method, which should inform us of being closed via our
  // |Close()|. Thus these should already be null.
  DCHECK(!endpoints_[0]);
  DCHECK(!endpoints_[1]);
}

MojoResult MessagePipe::EnqueueMessage(
    unsigned port,
    scoped_ptr<MessageInTransit> message,
    std::vector<DispatcherTransport>* transports) {
  DCHECK(port == 0 || port == 1);
  DCHECK(message);

  if (message->type() == MessageInTransit::kTypeMessagePipe) {
    DCHECK(!transports);
    return HandleControlMessage(port, message.Pass());
  }

  DCHECK_EQ(message->type(), MessageInTransit::kTypeMessagePipeEndpoint);

  base::AutoLock locker(lock_);
  DCHECK(endpoints_[GetPeerPort(port)]);

  // The destination port need not be open, unlike the source port.
  if (!endpoints_[port])
    return MOJO_RESULT_FAILED_PRECONDITION;

  if (transports) {
    MojoResult result = AttachTransportsNoLock(port, message.get(), transports);
    if (result != MOJO_RESULT_OK)
      return result;
  }

  // The endpoint's |EnqueueMessage()| may not report failure.
  endpoints_[port]->EnqueueMessage(message.Pass());
  return MOJO_RESULT_OK;
}

MojoResult MessagePipe::AttachTransportsNoLock(
    unsigned port,
    MessageInTransit* message,
    std::vector<DispatcherTransport>* transports) {
  DCHECK(!message->has_dispatchers());

  // You're not allowed to send either handle to a message pipe over the message
  // pipe, so check for this. (The case of trying to write a handle to itself is
  // taken care of by |Core|. That case kind of makes sense, but leads to
  // complications if, e.g., both sides try to do the same thing with their
  // respective handles simultaneously. The other case, of trying to write the
  // peer handle to a handle, doesn't make sense -- since no handle will be
  // available to read the message from.)
  for (size_t i = 0; i < transports->size(); i++) {
    if (!(*transports)[i].is_valid())
      continue;
    if ((*transports)[i].GetType() == Dispatcher::kTypeMessagePipe) {
      MessagePipeDispatcherTransport mp_transport((*transports)[i]);
      if (mp_transport.GetMessagePipe() == this) {
        // The other case should have been disallowed by |Core|. (Note: |port|
        // is the peer port of the handle given to |WriteMessage()|.)
        DCHECK_EQ(mp_transport.GetPort(), port);
        return MOJO_RESULT_INVALID_ARGUMENT;
      }
    }
  }

  // Clone the dispatchers and attach them to the message. (This must be done as
  // a separate loop, since we want to leave the dispatchers alone on failure.)
  scoped_ptr<DispatcherVector> dispatchers(new DispatcherVector());
  dispatchers->reserve(transports->size());
  for (size_t i = 0; i < transports->size(); i++) {
    if ((*transports)[i].is_valid()) {
      dispatchers->push_back(
          (*transports)[i].CreateEquivalentDispatcherAndClose());
    } else {
      LOG(WARNING) << "Enqueueing null dispatcher";
      dispatchers->push_back(nullptr);
    }
  }
  message->SetDispatchers(dispatchers.Pass());
  return MOJO_RESULT_OK;
}

MojoResult MessagePipe::HandleControlMessage(
    unsigned /*port*/,
    scoped_ptr<MessageInTransit> message) {
  LOG(WARNING) << "Unrecognized MessagePipe control message subtype "
               << message->subtype();
  return MOJO_RESULT_UNKNOWN;
}

}  // namespace system
}  // namespace mojo
