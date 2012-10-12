#include <functional>

#include "../../../quorum/quorum.hpp"
#include "../../../paxos_context.hpp"
#include "../../../command.hpp"
#include "../../../parser.hpp"
#include "../../../tcp_connection.hpp"
#include "../../../util/debug.hpp"

#include "strategy.hpp"

namespace paxos { namespace detail { namespace strategy { namespace basic_paxos { namespace protocol {

/*! virtual */ void
strategy::initiate (      
   tcp_connection_ptr           client_connection,
   detail::command const &      command,
   detail::quorum::quorum &     quorum,
   detail::paxos_context &      global_state,
   queue_guard_type             queue_guard) const
{
   PAXOS_ASSERT (quorum.who_is_our_leader () == quorum.our_endpoint ());

   /*!
     At the start of any request, we should, as defined in the Paxos protocol, increment
     our current proposal id.
    */
   ++(global_state.proposal_id ());

   /*!
     Keeps track of the current state / which servers have responded, etc.
    */
   boost::shared_ptr <struct state> state (new struct state ());

   /*!
     Note that this will ensure the queue guard is in place for as long as the request is
     being processed.
    */
   state->queue_guard = queue_guard;

   /*!
     Tell all nodes within this quorum to prepare this request.
    */
   for (boost::asio::ip::tcp::endpoint const & endpoint : quorum.live_server_endpoints ())
   {
      detail::quorum::server & server = quorum.lookup_server (endpoint);

      PAXOS_DEBUG ("sending paxos request to server " << endpoint);

      send_prepare (client_connection,
                    command,
                    quorum.our_endpoint (),
                    server.endpoint (),
                    server.connection (),
                    boost::ref (global_state),
                    command.workload (),
                    state);
   }
}


/*! virtual */ void
strategy::send_prepare (
   tcp_connection_ptr                           client_connection,
   detail::command const &                      client_command,
   boost::asio::ip::tcp::endpoint const &       leader_endpoint,   
   boost::asio::ip::tcp::endpoint const &       follower_endpoint,
   tcp_connection_ptr                           follower_connection,
   detail::paxos_context &                      global_state,
   std::string const &                          byte_array,
   boost::shared_ptr <struct state>             state) const
{

   /*!
     Ensure to claim an entry in our state so that in a later point in time, we know
     which servers have responded and which don't.
    */
   PAXOS_ASSERT (state->connections.find (follower_endpoint) == state->connections.end ());
   state->connections[follower_endpoint] = follower_connection;


   /*!
     And now always send a 'prepare' proposal to the server. This will validate our proposal
     id at the other server's end.
    */
   command command;
   command.set_type (command::type_request_prepare);
   command.set_proposal_id (global_state.proposal_id ());
   command.set_host_endpoint (leader_endpoint);


   PAXOS_DEBUG ("step2 writing command");

   follower_connection->command_dispatcher ().write (command);

   PAXOS_DEBUG ("step2 reading command");   

   /*!
     We expect either an 'ack' or a 'reject' response to this command.
    */
   follower_connection->command_dispatcher ().read (
      command,
      std::bind (&strategy::receive_promise,
                 this,
                 client_connection,
                 client_command,
                 leader_endpoint,
                 follower_endpoint,
                 follower_connection,
                 boost::ref (global_state),
                 byte_array,
                 std::placeholders::_1,
                 state));
}

/*! virtual */ void
strategy::prepare (      
   tcp_connection_ptr           leader_connection,
   detail::command const &      command,
   detail::quorum::quorum &     quorum,
   detail::paxos_context &      state) const
{
   detail::command response;

   PAXOS_DEBUG ("self = " << quorum.our_endpoint () << ", "
                "state.proposal_id () = " << state.proposal_id () << ", "
                "command.proposal_id () = " << command.proposal_id ());

   if (command.host_endpoint () == quorum.our_endpoint ())
   {
      /*!
        This is the leader sending the 'prepare' to itself, always ACK
       */
      response.set_type (command::type_request_promise);
   }
   else if (command.proposal_id () > state.proposal_id ())
   {
      state.proposal_id () = command.proposal_id ();
      response.set_type (command::type_request_promise);
   }
   else
   {
      response.set_type (command::type_request_fail);
   }

   response.set_proposal_id (state.proposal_id ());

   PAXOS_DEBUG ("step3 writing command");   

   leader_connection->command_dispatcher ().write (command,
                                                   response);
}


/*! virtual */ void
strategy::receive_promise (
   tcp_connection_ptr                           client_connection,
   detail::command                              client_command,
   boost::asio::ip::tcp::endpoint const &       leader_endpoint,
   boost::asio::ip::tcp::endpoint const &       follower_endpoint,
   tcp_connection_ptr                           follower_connection,
   detail::paxos_context &                      global_state,
   std::string                                  byte_array,
   detail::command const &                      command,
   boost::shared_ptr <struct state>             state) const
{
   PAXOS_ASSERT (state->connections[follower_endpoint] == follower_connection);

   PAXOS_DEBUG ("step4 received command");

   switch (command.type ())
   {
         case command::type_request_promise:
            PAXOS_ASSERT (command.proposal_id () == global_state.proposal_id ());
            state->accepted[follower_endpoint] = response_ack;
            break;

         case command::type_request_fail:
            state->accepted[follower_endpoint] = response_reject;
            
            /*!
              Since our follower has rejected this based on our proposal id, it makes
              sense to ensure that the next proposal id we will use it as least higher
              than this follower's proposal id.
             */
            global_state.proposal_id () = std::max (global_state.proposal_id (),
                                                    command.proposal_id ());

            break;

         default:
            /*!
              Protocol error!
            */
            PAXOS_UNREACHABLE ();
   };



   bool everyone_responded = (state->connections.size () == state->accepted.size ());

   bool everyone_promised  = true;
   for (auto const & i : state->accepted)
   {
      everyone_promised  = everyone_promised && i.second == response_ack;
   }

   PAXOS_DEBUG ("step4 everyone_responsed = " << everyone_responded << ", everyone_promised = " << everyone_promised);


   if (everyone_responded == true && everyone_promised == false)
   {
      /*!
        This means that every host has given a response, yet not everyone has actually
        accepted our proposal id. 

        We will send an error command to the client informing about the failed command.
       */
      PAXOS_DEBUG ("step4 writing error command");   

      detail::command response;
      response.set_type (command::type_request_error);
      response.set_error_code (paxos::error_incorrect_proposal);

      client_connection->command_dispatcher ().write (client_command,
                                                      response);
   }
   else if (everyone_responded == true && everyone_promised == true)
   {
      /*!
        This means that all nodes in the quorum have responded, and they all agree with
        the proposal id, yay!

        Now that all these nodes have promised to accept any request with the specified
        proposal id, let's send them an accept command.
       */

      for (auto & i : state->connections)
      {
         send_accept (client_connection,
                      client_command,
                      leader_endpoint,
                      i.first,
                      i.second,
                      boost::ref (global_state),
                      byte_array,
                      state);
      }
   }
}

/*! virtual */ void
strategy::send_accept (
   tcp_connection_ptr                           client_connection,
   detail::command const &                      client_command,
   boost::asio::ip::tcp::endpoint const &       leader_endpoint,
   boost::asio::ip::tcp::endpoint const &       follower_endpoint,
   tcp_connection_ptr                           follower_connection,
   detail::paxos_context &                      global_state,
   std::string const &                          byte_array,
   boost::shared_ptr <struct state>             state) const
{
   
   PAXOS_ASSERT (state->connections[follower_endpoint] == follower_connection);
   PAXOS_ASSERT (state->accepted[follower_endpoint] == response_ack);


   command command;
   command.set_type (command::type_request_accept);
   command.set_proposal_id (global_state.proposal_id ());
   command.set_host_endpoint (leader_endpoint);
   command.set_workload (byte_array);

   PAXOS_DEBUG ("step5 writing command");   

   follower_connection->command_dispatcher ().write (command);
   
   PAXOS_DEBUG ("step5 reading command");   

   /*!
     We expect a response to this command.
    */
   follower_connection->command_dispatcher ().read (
      command,
      std::bind (&strategy::receive_accepted,
                 this,
                 client_connection,
                 client_command,
                 follower_endpoint,
                 std::placeholders::_1,
                 state));

}


/*! virtual */ void
strategy::accept (      
   tcp_connection_ptr           leader_connection,
   detail::command const &      command,
   detail::quorum::quorum &     quorum,
   detail::paxos_context &      state) const
{
   detail::command response;
   
   /*!
     If the proposal id's do not match, something went terribly wrong! Most likely a switch
     of leaders during the operation.
    */
   if (command.proposal_id () != state.proposal_id ())
   {
      response.set_type (command::type_request_fail);
   }
   else
   {
      PAXOS_DEBUG ("server " << quorum.our_endpoint () << " calling processor "
                   "with workload = '" << command.workload () << "'");
      response.set_type (command::type_request_accepted);
      response.set_workload (
         state.processor () (command.workload ()));
   }

   PAXOS_DEBUG ("step6 writing command");   

   leader_connection->command_dispatcher ().write (command,
                                                   response);
}


/*! virtual */ void
strategy::receive_accepted (
   tcp_connection_ptr                           client_connection,
   detail::command                              client_command,
   boost::asio::ip::tcp::endpoint const &       follower_endpoint,
   detail::command const &                      command,
   boost::shared_ptr <struct state>             state) const
{
   PAXOS_ASSERT (state->accepted[follower_endpoint] == response_ack);
   PAXOS_ASSERT (state->responses.find (follower_endpoint) == state->responses.end ());

   /*!
     Let's store the response we received!
   */
   state->responses[follower_endpoint] = command.workload ();

   if (command.type () == command::type_request_fail)
   {
      state->accepted[follower_endpoint] = response_reject;
   }

   std::string workload;

   PAXOS_ASSERT (state->connections.size () == state->accepted.size ());
   if (state->connections.size () == state->responses.size ())
   {
      bool all_same_response = true;
      bool everyone_promised = true;

      for (auto const & i : state->accepted)
      {
         everyone_promised  = everyone_promised && i.second == response_ack;
      }
      
      for (auto const & i : state->responses)
      {
         /*!
           One of the requirements of our protocol is that if one node N1 replies
           to proposal P with response R, node N2 must have the exact same response
           for the same proposal.

           The code below validates this requirement.
         */
         if (workload.empty () == true)
         {
            workload = i.second;
         }
         else if (workload != i.second)
         {
            all_same_response = false;
         }
      }

      if (everyone_promised == true
          && all_same_response == true)
      {
         /*!
           Send a copy of the last command to the client, since the workload should be the
           same for all responses.
         */
         PAXOS_DEBUG ("step7 writing command");   

         detail::command command_copy (command);

         client_connection->command_dispatcher ().write (client_command,
                                                         command_copy);
      }
      else
      {
         /*!
           We're going to inform the client about an inconsistent response here. Perhaps
           he can recover from there.
         */
         PAXOS_DEBUG ("step7 writing error command");   

         detail::command response;
         response.set_type (command::type_request_error);

         if (everyone_promised == false)
         {
            response.set_error_code (paxos::error_incorrect_proposal);
         }
         else if (all_same_response == false)
         {
            response.set_error_code (paxos::error_inconsistent_response);
         }

         client_connection->command_dispatcher ().write (client_command,
                                                         response);
      }
   }
}


}; }; }; }; };