/*!
  Copyright (c) 2012, Leon Mergen, all rights reserved.
 */

#ifndef LIBPAXOS_CPP_DURABLE_STORAGE_HPP
#define LIBPAXOS_CPP_DURABLE_STORAGE_HPP

#include <stdint.h>

#include <map>
#include <string>

#include <boost/function.hpp>

namespace paxos { namespace durable {

/*!
  \brief Provides base class for durable storage components, which acts as the collective 
         memory of the quorum
 */
class storage
{
public:

   storage ();

   /*!
     \brief Destructor
    */
   virtual ~storage ();

   /*!
     \brief Sets the minimum history to keep in storage

     The storage component should never clean logs if we have less than this amount
     of logs in history.
    */
   void
   set_history_size (
      int64_t  history_size);

   /*!
     \brief Access to the amount of values to keep in storage
    */
   int64_t
   history_size () const;
   

   /*!
     \brief Accepts a new value

     This function calls store (), and if the size of the history is growing too large
     calls for a cleanup.
    */
   void
   accept (
      int64_t                   proposal_id,
      std::string const &       byte_array);

   /*!
     \brief Looks up all recently accepted values higher than with \c proposal_id

     This function does not necessarily need to return all values. In fact, it is preferred if
     a large catch-up is retrieved in small batches, so that a catch-up can occur gradually 
     instead of in a single paxos round.
   */
   virtual std::map <int64_t, std::string>
   retrieve (
      int64_t                   proposal_id) = 0;

   /*!
     \brief Looks up the highest proposal id currently stored
     \returns Returns highest proposal id in history, or 0 if no previous proposals are stored
    */
   virtual int64_t
   highest_proposal_id () = 0;

protected:

   /*!
     \brief Stores an accepted value
     \param proposal_id The id of the proposal to store
     \param byte_array  The value that is associated with the proposal

     \par Preconditions
     
     highest_proposal_id () == (proposal_id - 1)

     \par Postconditions

     highest_proposal_id () == proposal_id

    */
   virtual void
   store (
      int64_t                   proposal_id,
      std::string const &       byte_array) = 0;

   /*!
     \brief Remove history for proposals with an id lower than \c proposal_id
     \param proposal_id The proposal_id to check for
    */
   virtual void
   remove (
      int64_t                   proposal_id) = 0;

private:

   int64_t      history_size_;

};

} }

#endif  //! LIBPAXOS_CPP_DURABLE_STORAGE_HPP
