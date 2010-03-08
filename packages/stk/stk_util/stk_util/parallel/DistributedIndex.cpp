/*------------------------------------------------------------------------*/
/*                 Copyright 2010 Sandia Corporation.                     */
/*  Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive   */
/*  license for use of this work by or on behalf of the U.S. Government.  */
/*  Export of this program may require a license from the                 */
/*  United States Government.                                             */
/*------------------------------------------------------------------------*/


#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <limits>
#include <stdint.h>
#include <stk_util/parallel/ParallelComm.hpp>
#include <stk_util/parallel/DistributedIndex.hpp>

namespace stk {
namespace parallel {

//----------------------------------------------------------------------

namespace {

struct KeyProcLess {

  bool operator()( const DistributedIndex::KeyProc & lhs ,
                   const DistributedIndex::KeyProc & rhs ) const
  {
    return lhs.first != rhs.first ? lhs.first  < rhs.first
                                  : lhs.second < rhs.second ;
  }

  bool operator()( const DistributedIndex::KeyProc & lhs ,
                   const DistributedIndex::KeyType & rhs ) const
  { return lhs.first < rhs ; }

};

void sort_unique( std::vector<DistributedIndex::KeyProc> & key_usage )
{
  std::vector<DistributedIndex::KeyProc>::iterator
    i = key_usage.begin() ,
    j = key_usage.end() ;

  std::sort( i , j , KeyProcLess() );

  i = std::unique( i , j );

  key_usage.erase( i , j );
}

}

//----------------------------------------------------------------------

enum { DISTRIBUTED_INDEX_CHUNK_BITS = 12 }; ///< Each chunk is 4096 keys

enum { DISTRIBUTED_INDEX_CHUNK_SIZE =
         size_t(1) << DISTRIBUTED_INDEX_CHUNK_BITS };


inline
DistributedIndex::ProcType
DistributedIndex::to_which_proc( const DistributedIndex::KeyType & key ) const
{ return ( key >> DISTRIBUTED_INDEX_CHUNK_BITS ) % m_comm_size ; }

//----------------------------------------------------------------------

DistributedIndex::~DistributedIndex() {}

DistributedIndex::DistributedIndex (
  ParallelMachine comm ,
  const std::vector<KeySpan> & partition_bounds )
  : m_comm( comm ),
    m_comm_rank( parallel_machine_rank( comm ) ),
    m_comm_size( parallel_machine_size( comm ) ),
    m_span_count(0),
    m_key_span(),
    m_chunk_first(),
    m_key_usage()
{
  unsigned info[2] ;
  info[0] = partition_bounds.size();
  info[1] = 0 ;

  // Check each span for validity

  for ( std::vector<KeySpan>::const_iterator
        i = partition_bounds.begin() ; i != partition_bounds.end() ; ++i ) {
    if ( i->second < i->first ||
         ( i != partition_bounds.begin() && i->first <= (i-1)->second ) ) {
      info[1] = 1 ;
    }
  }

#if defined( STK_HAS_MPI )
  MPI_Bcast( info , 2 , MPI_UNSIGNED , 0 , comm );

  if ( 0 < info[0] ) {
    m_key_span.resize( info[0] );
    if ( 0 == parallel_machine_rank( comm ) ) {
      m_key_span = partition_bounds ;
    }
    MPI_Bcast( & m_key_span[0], info[0] * sizeof(KeySpan), MPI_BYTE, 0, comm );
  }
#else
  m_key_span = partition_bounds ;
#endif

  if ( info[1] ) {
    std::ostringstream msg ;
    msg << "sierra::parallel::DistributedIndex ctor( comm , " ;

    for ( std::vector<KeySpan>::const_iterator
          i = partition_bounds.begin() ; i != partition_bounds.end() ; ++i ) {
      msg << " ( min = " << i->first << " , max = " << i->second << " )" ;
    }
    msg << " ) contains invalid span of keys" ;
    throw std::runtime_error( msg.str() );
  }

  m_span_count = info[0] ;

  if ( 0 == m_span_count ) {
    m_key_span.push_back(
      KeySpan( std::numeric_limits<KeyType>::min(),
               std::numeric_limits<KeyType>::max() ) );
    m_span_count = 1 ;
  }

  m_chunk_first.resize( m_span_count );

  for ( size_t i = 0 ; i < m_span_count ; ++i ) {
    const KeyType key_span_first = m_key_span[i].first ;
    size_t chunk_iter = 0 ;

    while ( m_comm_rank !=
            to_which_proc( key_span_first +
                           chunk_iter * DISTRIBUTED_INDEX_CHUNK_SIZE ) ) {
      ++chunk_iter ;
    }
    m_chunk_first[ i ] = chunk_iter ;
  }
}

//----------------------------------------------------------------------
//----------------------------------------------------------------------

namespace {

void query_pack( const std::vector<DistributedIndex::KeyProc> & key_usage ,
                 const std::vector<DistributedIndex::KeyProc> & request ,
                 CommAll & all )
{
  std::vector<DistributedIndex::KeyProc>::const_iterator i = key_usage.begin();

  for ( std::vector<DistributedIndex::KeyProc>::const_iterator
        k =  request.begin() ;
        k != request.end() &&
        i != key_usage.end() ; ++k ) {

    for ( ; i != key_usage.end() && i->first < k->first ; ++i );

    for ( std::vector<DistributedIndex::KeyProc>::const_iterator j = i ;
          j != key_usage.end() && j->first == k->first ; ++j ) {
      all.send_buffer( k->second ).pack<DistributedIndex::KeyProc>( *j );
    }
  }
}

}

void DistributedIndex::query(
  const std::vector<DistributedIndex::KeyProc> & request ,
        std::vector<DistributedIndex::KeyProc> & sharing_of_keys ) const
{
  sharing_of_keys.clear();

  CommAll all( m_comm );

  query_pack( m_key_usage , request , all ); // Sizing

  all.allocate_buffers( m_comm_size / 4 , false );

  query_pack( m_key_usage , request , all ); // Packing

  all.communicate();

  for ( ProcType p = 0 ; p < m_comm_size ; ++p ) {
    CommBuffer & buf = all.recv_buffer( p );
    while ( buf.remaining() ) {
      KeyProc kp ;
      buf.unpack( kp );
      sharing_of_keys.push_back( kp );
    }
  }

  std::sort( sharing_of_keys.begin() , sharing_of_keys.end() );
}


void DistributedIndex::query(
  std::vector<DistributedIndex::KeyProc> & sharing_of_local_keys ) const
{
  query( m_key_usage , sharing_of_local_keys );
}

void DistributedIndex::query(
  const std::vector<DistributedIndex::KeyType> & keys ,
        std::vector<DistributedIndex::KeyProc> & sharing_keys ) const
{
  std::vector<KeyProc> request ;

  {
    CommAll all( m_comm );

    for ( std::vector<KeyType>::const_iterator
          k = keys.begin() ; k != keys.end() ; ++k ) {
      all.send_buffer( to_which_proc( *k ) ).pack<KeyType>( *k );
    }

    all.allocate_buffers( m_comm_size / 4 , false );

    for ( std::vector<KeyType>::const_iterator
          k = keys.begin() ; k != keys.end() ; ++k ) {
      all.send_buffer( to_which_proc( *k ) ).pack<KeyType>( *k );
    }

    all.communicate();

    for ( ProcType p = 0 ; p < m_comm_size ; ++p ) {
      CommBuffer & buf = all.recv_buffer( p );
      KeyProc kp ;
      kp.second = p ;
      while ( buf.remaining() ) {
        buf.unpack<KeyType>( kp.first );
        request.push_back( kp );
      }
    }
  }

  sort_unique( request );

  query( request , sharing_keys );
}

//----------------------------------------------------------------------
//----------------------------------------------------------------------

namespace {

struct RemoveKeyProc {

  bool operator()( const DistributedIndex::KeyProc & kp ) const
    { return kp.second < 0 ; }

  static void mark( std::vector<DistributedIndex::KeyProc> & key_usage ,
                    const DistributedIndex::KeyProc & kp )
  {
    std::vector<DistributedIndex::KeyProc>::iterator
      i = std::lower_bound( key_usage.begin(),
                            key_usage.end(), kp.first , KeyProcLess() );
    while ( i != key_usage.end() && kp != *i ) { ++i ; }
    if ( i != key_usage.end() && kp == *i ) {
      i->second = -1 ;
    }
  }

  static void clean( std::vector<DistributedIndex::KeyProc> & key_usage )
  {
    std::vector<DistributedIndex::KeyProc>::iterator end =
      std::remove_if( key_usage.begin() , key_usage.end() , RemoveKeyProc() );
    key_usage.erase( end , key_usage.end() );
  }
};

}

void DistributedIndex::update_keys(
  const std::vector<DistributedIndex::KeyType> & add_new_keys ,
  const std::vector<DistributedIndex::KeyType> & remove_existing_keys )
{
  std::vector<unsigned long> count_remove( m_comm_size , (unsigned long)0 );
  std::vector<unsigned long> count_add(    m_comm_size , (unsigned long)0 );

  for ( std::vector<KeyType>::const_iterator
        i = remove_existing_keys.begin();
        i != remove_existing_keys.end(); ++i ) {
    const ProcType p = to_which_proc( *i );
    if ( p != m_comm_rank ) {
      ++( count_remove[ to_which_proc( *i ) ] );
    }
  }

  size_t local_bad_input = 0 ;

  for ( std::vector<KeyType>::const_iterator
        i = add_new_keys.begin();
        i != add_new_keys.end(); ++i ) {

    // Input key must be within one of the spans:

    std::vector<KeySpan>::const_iterator j  = m_key_span.begin();
    std::vector<KeySpan>::const_iterator je = m_key_span.end();
    for ( ; j != je ; ++j ) {
      if ( j->first <= *i && *i <= j->second ) { break ; }
    }
    if ( j == je ) { ++local_bad_input ; }

    // Count

    const ProcType p = to_which_proc( *i );
    if ( p != m_comm_rank ) {
      ++( count_add[ to_which_proc( *i ) ] );
    }
  }

  if ( 0 < local_bad_input ) {
    // If this process knows it will throw
    // then don't bother communicating the add and remove requests.
    count_remove.clear();
    count_add.clear();
  }

  CommAll all( m_comm );

  // Sizing and add_new_keys bounds checking:

  for ( int p = 0 ; p < m_comm_size ; ++p ) {
    if ( count_remove[p] || count_add[p] ) {
      CommBuffer & buf = all.send_buffer( p );
      buf.skip<unsigned long>( 1 );
      buf.skip<KeyType>( count_remove[p] );
      buf.skip<KeyType>( count_add[p] );
    }
  }

  // Allocate buffers and perform a global
  const bool symmetry_flag = false ;
  const bool error_flag = 0 < local_bad_input ;

  bool global_bad_input =
    all.allocate_buffers( m_comm_size / 4, symmetry_flag , error_flag );

  if ( global_bad_input ) {
    std::ostringstream msg ;

    if ( 0 < local_bad_input ) {
      msg << "stk::parallel::DistributedIndex::update_keys ERROR Given "
          << local_bad_input << " of " << add_new_keys.size()
          << " add_new_keys outside of any span" ;
    }

    throw std::runtime_error( msg.str() );
  }

  // Packing:

  for ( int p = 0 ; p < m_comm_size ; ++p ) {
    if ( count_remove[p] || count_add[p] ) {
      all.send_buffer( p ).pack<unsigned long>( count_remove[p] );
    }
  }

  for ( std::vector<KeyType>::const_iterator
        i = remove_existing_keys.begin();
        i != remove_existing_keys.end(); ++i ) {
    const ProcType p = to_which_proc( *i );
    if ( p != m_comm_rank ) {
      all.send_buffer( p ).pack<KeyType>( *i );
    }
  }

  for ( std::vector<KeyType>::const_iterator
        i = add_new_keys.begin();
        i != add_new_keys.end(); ++i ) {
    const ProcType p = to_which_proc( *i );
    if ( p != m_comm_rank ) {
      all.send_buffer( p ).pack<KeyType>( *i );
    }
  }

  all.communicate();

  //------------------------------
  // Remove for local keys

  for ( std::vector<KeyType>::const_iterator
        i = remove_existing_keys.begin();
        i != remove_existing_keys.end(); ++i ) {
    const ProcType p = to_which_proc( *i );
    if ( p == m_comm_rank ) {
      KeyProc kp( *i , p );
      RemoveKeyProc::mark( m_key_usage , kp );
    }
  }

  // Unpack the remove key and find it.
  // Set the process to a negative value for subsequent removal.

  for ( int p = 0 ; p < m_comm_size ; ++p ) {
    CommBuffer & buf = all.recv_buffer( p );
    if ( buf.remaining() ) {
      unsigned long remove_count = 0 ;

      KeyProc kp ;

      kp.second = p ;

      buf.unpack<unsigned long>( remove_count );

      for ( ; 0 < remove_count ; --remove_count ) {
        buf.unpack<KeyType>( kp.first );

        RemoveKeyProc::mark( m_key_usage , kp );
      }
    }
  }

  RemoveKeyProc::clean( m_key_usage );

  //------------------------------
  // Append for local keys

  for ( std::vector<KeyType>::const_iterator
        i = add_new_keys.begin();
        i != add_new_keys.end(); ++i ) {

    const ProcType p = to_which_proc( *i );
    if ( p == m_comm_rank ) {
      KeyProc kp( *i , p );
      m_key_usage.push_back( kp );
    }
  }

  // Unpack and append for remote keys:
  for ( int p = 0 ; p < m_comm_size ; ++p ) {
    CommBuffer & buf = all.recv_buffer( p );

    KeyProc kp ;

    kp.second = p ;

    while ( buf.remaining() ) {
      buf.unpack<KeyType>( kp.first );
      m_key_usage.push_back( kp );
    }
  }

  sort_unique( m_key_usage );
  //------------------------------
}

//----------------------------------------------------------------------
//----------------------------------------------------------------------
// For efficient communication merging three reductions:
// 1) current global counts of keys used          [ m_span_count ]
// 2) current global counts of new keys requested [ m_span_count ]
// 3) an input error flag.

void DistributedIndex::generate_new_keys_global_counts(
  const std::vector<size_t>  & requests ,
        std::vector<size_t>  & requests_global_sum ,
        std::vector<size_t>  & existing_global_sum ) const
{
  bool bad_request = m_span_count != requests.size();

  std::ostringstream error_msg ;

  error_msg
  << "sierra::parallel::DistributedIndex::generate_new_keys_global_counts( " ;

  std::vector<unsigned long>
    local_counts( 2 * m_span_count + 1 , (unsigned long) 0 ),
    global_counts( 2 * m_span_count + 1 , (unsigned long) 0 );

  // Count unique keys in each span

  {
    std::vector<KeyProc>::const_iterator j = m_key_usage.begin();

    for ( size_t i = 0 ; i < m_span_count && j != m_key_usage.end() ; ++i ) {
      const KeyType key_span_last = m_key_span[i].second ;
      size_t count = 0 ;
      while ( j != m_key_usage.end() && j->first <= key_span_last ) {
        const KeyType key = j->first ;
        while ( key == j->first ) { ++j ; }
        ++count ;
      }
      local_counts[i] = count ;
    }

    for ( size_t i = 0 ; i < m_span_count ; ++i ) {
      local_counts[i + m_span_count] = i < requests.size() ? requests[i] : 0 ;
    }
  }

  // Append the error check to this communication to avoid
  // and extra reduction operation.
  local_counts[ 2 * m_span_count ] = m_span_count != requests.size();

#if defined( STK_HAS_MPI )
  MPI_Allreduce( & local_counts[0] , & global_counts[0] ,
                 2 * m_span_count + 1 , MPI_UNSIGNED_LONG ,
                 MPI_SUM , m_comm );
#else
  global_counts = local_counts ;
#endif

  bad_request = global_counts[2*m_span_count] != 0 ;

  if ( bad_request ) {
    if ( m_span_count != requests.size() ) {
      error_msg << " requests.size() = " << requests.size()
                << " != " << m_span_count << " )" ;
    }
  }

  if ( ! bad_request ) {
    for ( unsigned i = 0 ; i < m_span_count ; ++i ) {
      const size_t span_available =
        ( 1 + m_key_span[i].second - m_key_span[i].first ) - global_counts[i] ;

      const size_t span_requested = global_counts[ i + m_span_count ];

      if ( span_available < span_requested ) {
        bad_request = true ;
        error_msg << " global_sum( request[" << i << "] ) = "
                  << span_requested
                  << " > global_sum( span_available ) = "
                  << span_available ;
      }
    }
  }

  if ( bad_request ) {
    throw std::runtime_error( error_msg.str() );
  }

  existing_global_sum.assign( global_counts.begin() ,
                              global_counts.begin() + m_span_count );

  requests_global_sum.assign( global_counts.begin() + m_span_count ,
                              global_counts.begin() + m_span_count * 2 );
}

//--------------------------------------------------------------------
//--------------------------------------------------------------------

void DistributedIndex::generate_new_keys_local_planning(
  const std::vector<size_t>   & existing_global_sum ,
  const std::vector<size_t>   & requests_global_sum ,
  const std::vector<size_t>   & requests_local ,
        std::vector<long>     & new_request ,
        std::vector<KeyType>  & requested_keys ,
        std::vector<KeyType>  & contrib_keys ) const
{
  new_request.assign( m_span_count , long(0) );

  contrib_keys.clear();

  std::vector<KeyProc>::const_iterator j = m_key_usage.begin();

  for ( size_t i = 0 ; i < m_span_count ; ++i ) {
    const size_t final_key_count =
      existing_global_sum[i] + requests_global_sum[ i ];

    const KeyType key_span_first = m_key_span[i].first ;
    const KeyType key_global_max = key_span_first + final_key_count - 1 ;

    const size_t init_size = contrib_keys.size();

    const size_t chunk_inc = m_comm_size * DISTRIBUTED_INDEX_CHUNK_SIZE ;

    for ( KeyType key_begin = key_span_first +
                  m_chunk_first[i] * DISTRIBUTED_INDEX_CHUNK_SIZE ;
          key_begin <= key_global_max ; key_begin += chunk_inc ) {

      // What is the first key of the chunk
      KeyType key_iter = key_begin ;

      // What is the last key belonging to this process' chunk
      const KeyType key_last =
        std::min( key_begin + DISTRIBUTED_INDEX_CHUNK_SIZE - 1 , key_global_max );

      // Jump into the sorted used key vector to
      // the key which may be contributed

      j = std::lower_bound( j, m_key_usage.end(), key_iter, KeyProcLess() );
      // now know:  j == m_key_usage.end() OR
      //            key_iter <= j->first

      for ( ; key_iter <= key_last ; ++key_iter ) {
        if ( j == m_key_usage.end() || key_iter < j->first ) {
          // The current attempt 'key_iter' is not used, contribute it.
          contrib_keys.push_back( key_iter );
        }
        else { // j != m_key_usage.end() && key_iter == j->first
          // The current attempt 'key_iter' is already used,
          // increment the used-iterator to its next key value.
          while ( j != m_key_usage.end() && key_iter == j->first ) {
            ++j ;
          }
        }
      }
    }

    // Determine which local keys will be contributed,
    // keeping what this process could use from the contribution.
    // This can reduce the subsequent communication load when
    // donating keys to another process.

    const size_t this_contrib = contrib_keys.size() - init_size ;

    // How many keys will this process keep:
    const size_t keep = std::min( requests_local[i] , this_contrib );

    // Take the kept keys from the contributed key vector.
    requested_keys.insert( requested_keys.end() ,
                           contrib_keys.end() - keep ,
                           contrib_keys.end() );

    contrib_keys.erase( contrib_keys.end() - keep ,
                        contrib_keys.end() );

    // New request is positive for needed keys or negative for donated keys
    new_request[i] = requests_local[i] - this_contrib ;
  }
}

//----------------------------------------------------------------------

void DistributedIndex::generate_new_keys_global_planning(
  const std::vector<KeyType> & contrib_keys ,
  const std::vector<long>    & new_request ,
        std::vector<long>    & my_donations ) const
{
  my_donations.assign( m_comm_size * m_span_count , long(0) );

  // Gather the global request plan for receiving and donating keys
  // Positive values for receiving, negative values for donating.

  std::vector<long> new_request_global( m_comm_size * m_span_count );

#if defined( STK_HAS_MPI )

  { // Gather requests into per-process spans
    // MPI doesn't do 'const' in its interface, but the send buffer is const
    void * send_buf = const_cast<void*>( (void *)( & new_request[0] ));
    void * recv_buf = & new_request_global[0] ;
    MPI_Allgather( send_buf , m_span_count , MPI_LONG ,
                   recv_buf , m_span_count , MPI_LONG , m_comm );
  }
#else
  new_request_global = new_request ;
#endif

  // Now have the global receive & donate plan.
  //--------------------------------------------------------------------
  // Generate my donate plan from the global receive & donate plan.

  std::vector<KeyType>::const_iterator ikey = contrib_keys.begin();

  for ( unsigned i = 0 ; i < m_span_count ; ++i ) {

    if ( new_request[i] < 0 ) { // This process is donating on this span
      long my_total_donate = - new_request[i] ;

      long previous_donate = 0 ;

      // Count what previous processes have donated:
      for ( int p = 0 ; p < m_comm_rank ; ++p ) {
        const long new_request_p = new_request_global[ p * m_span_count + i ] ;
        if ( new_request_p < 0 ) {
          previous_donate -= new_request_p ;
        }
      }

      // What the donation count will be with my donation:
      long end_donate = previous_donate + my_total_donate ;

      long previous_receive = 0 ;

      // Determine my donation to other processes (one to many).

      for ( int p = 0 ; p < m_comm_size && 0 < my_total_donate ; ++p ) {

        const long new_request_p = new_request_global[ p * m_span_count + i ];

        if ( 0 < new_request_p ) { // Process 'p' receives keys

          // Accumulation of requests:

          previous_receive += new_request_p ;

          if ( previous_donate < previous_receive ) {
            // I am donating to process 'p'
            const long n = std::min( previous_receive , end_donate )
                           - previous_donate ;

            my_donations[ p * m_span_count + i ] = n ;
            previous_donate += n ;
            my_total_donate -= n ;
          }
        }
      }
    }
  }
}

//--------------------------------------------------------------------

void DistributedIndex::generate_new_keys(
  const std::vector<size_t>                 & requests ,
        std::vector< std::vector<KeyType> > & requested_keys )
{
  //--------------------------------------------------------------------
  // Develop the plan:

  std::vector<size_t>  requests_global_sum ;
  std::vector<size_t>  existing_global_sum ;
  std::vector<long>    new_request ;
  std::vector<long>    my_donations ;
  std::vector<KeyType> contrib_keys ;
  std::vector<KeyType> new_keys ;

  // Verify input and generate global sum of
  // current key usage and requested new keys.
  // Throw a parallel consistent exception if the input is bad.

  generate_new_keys_global_counts( requests ,
                                   requests_global_sum ,
                                   existing_global_sum );

  //  No exception thrown means all inputs are good and parallel consistent

  // Determine which local keys will be contributed,
  // keeping what this process could use from the contribution.
  // This can reduce the subsequent communication load when
  // donating keys to another process.

  generate_new_keys_local_planning( existing_global_sum ,
                                    requests_global_sum ,
                                    requests ,
                                    new_request ,
                                    new_keys ,
                                    contrib_keys );

  // Determine where this process will be donating 'contrib_keys'
  generate_new_keys_global_planning( contrib_keys, new_request, my_donations );

  // Plan is done, communicate the new keys.
  //--------------------------------------------------------------------
  // Update counts by the number of keys this process is contributing.
  // Both kept and donated keys.

  // Add kept keys to this process' key index.
  // The key index is no longer properly ordered.
  // It must be sorted before completion,
  // but not until remotely donated keys are added.

  for ( std::vector<KeyType>::iterator
        ik  = new_keys.begin() ;
        ik != new_keys.end() ; ++ik ) {
    m_key_usage.push_back( KeyProc( *ik , m_comm_rank ) );
  }

  {
    size_t n = 0 ;
    for ( size_t i = 0 ; i < m_span_count ; ++i ) {
      for ( int p = 0 ; p < m_comm_size ; ++p ) {
        const size_t n_to_p = my_donations[ p * m_span_count + i ];
        if ( n_to_p ) {
          for ( size_t ik = 0; ik < n_to_p ; ++ik , ++n ) {
            m_key_usage.push_back( KeyProc( contrib_keys[n] , p ) );
          }
        }
      }
    }
  }

  std::sort( m_key_usage.begin() , m_key_usage.end() , KeyProcLess() );

  //--------------------------------------------------------------------

  CommAll all( m_comm );

  // Sizing

  for ( size_t i = 0 ; i < m_span_count ; ++i ) {
    for ( int p = 0 ; p < m_comm_size ; ++p ) {
      const size_t n_to_p = my_donations[ p * m_span_count + i ];
      if ( 0 < n_to_p ) {
        all.send_buffer(p).skip<KeyType>( n_to_p );
      }
    }
  }

  all.allocate_buffers( m_comm_size / 4 , false );

  // Packing

  {
    size_t n = 0 ;
    for ( size_t i = 0 ; i < m_span_count ; ++i ) {
      for ( int p = 0 ; p < m_comm_size ; ++p ) {
        const size_t n_to_p = my_donations[ p * m_span_count + i ];
        if ( 0 < n_to_p ) {
          all.send_buffer(p).pack<KeyType>( & contrib_keys[n] , n_to_p );
          n += n_to_p ;
        }
      }
    }
  }

  all.communicate();

  // Unpacking

  for ( int p = 0 ; p < m_comm_size ; ++p ) {
    CommBuffer & buf = all.recv_buffer( p );
    while ( buf.remaining() ) {
      KeyType key ;
      buf.unpack<KeyType>( key );
      new_keys.push_back( key );
    }
  }

  std::sort( new_keys.begin() , new_keys.end() );

  requested_keys.resize( m_span_count );

  size_t n = 0 ;
  for ( size_t i = 0 ; i < m_span_count ; ++i ) {
    requested_keys[i].assign( new_keys.begin() + n ,
                              new_keys.begin() + n + requests[i] );
    n += requests[i] ;
  }

  return ;
}

//----------------------------------------------------------------------

} // namespace util
} // namespace stk

