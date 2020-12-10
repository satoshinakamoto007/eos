#pragma once

#include <queue>

#include <fc/filesystem.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/datastream.hpp>
#include <fc/io/raw.hpp>
#include <fc/reflect/typename.hpp>
#include <fstream>
#include <b1/session/session.hpp>
#include <b1/session/session_variant.hpp>
#include <eosio/chain/exceptions.hpp>

namespace eosio::session {
   const uint32_t undo_stack_magic_number = 0x30510ABC;
   const uint32_t undo_stack_min_supported_version = 1;
   const uint32_t undo_stack_max_supported_version = 1;
   constexpr auto undo_stack_filename = "undo_stack.dat";

/// \brief Represents a container of pending sessions to be committed.
template <typename Session>
class undo_stack {
 public:
   using root_type          = Session;
   using session_type       = session<Session>;
   using variant_type       = session_variant<root_type, session_type>;
   using const_variant_type = session_variant<const root_type, const session_type>;

   /// \brief Constructor.
   /// \param head The session that the changes are merged into when commit is called.
   undo_stack(Session& head, const fc::path& datadir = {});
   undo_stack(const undo_stack&) = delete;
   undo_stack(undo_stack&&)      = default;
   ~undo_stack();

   undo_stack& operator=(const undo_stack&) = delete;
   undo_stack& operator=(undo_stack&&) = default;

   /// \brief Adds a new session to the top of the stack.
   void push();

   /// \brief Merges the changes of the top session into the session below it in the stack.
   void squash();

   /// \brief Pops the top session off the stack and discards the changes in that session.
   void undo();

   /// \brief Commits the sessions at the bottom of the stack up to and including the provided revision.
   /// \param revision The revision number to commit up to.
   /// \remarks Each time a session is push onto the stack, a revision is assigned to it.
   void commit(int64_t revision);

   bool   empty() const;
   size_t size() const;

   /// \brief The starting revision number of the stack.
   int64_t revision() const;

   /// \brief Sets the starting revision number of the stack.
   /// \remarks This can only be set when the stack is empty and it can't be set
   /// to value lower than the current revision.
   void revision(int64_t revision);

   /// \brief Returns the head session (the session at the top of the stack.
   variant_type top();

   /// \brief Returns the head session (the session at the top of the stack.
   const_variant_type top() const;

   /// \brief Returns the session at the bottom of the stack.
   /// \remarks This is the next session to be committed.
   variant_type bottom();

   /// \brief Returns the session at the bottom of the stack.
   /// \remarks This is the next session to be committed.
   const_variant_type bottom() const;

   void open();
   void close();

 private:
   int64_t                  m_revision{ 0 };
   Session*                 m_head;
   std::deque<session_type> m_sessions; // Need a deque so pointers don't become invalidated.  The session holds a
                                        // pointer to the parent internally.
   fc::path                 m_datadir;
};

template <typename Session>
undo_stack<Session>::undo_stack(Session& head, const fc::path& datadir) : m_head{ &head }, m_datadir{ datadir } {
   open();
}

template <typename Session>
undo_stack<Session>::~undo_stack() {
   close();

   for (auto& session : m_sessions) { session.undo(); }
}

template <typename Session>
void undo_stack<Session>::push() {
   if (m_sessions.empty()) {
      m_sessions.emplace_back(*m_head);
   } else {
      m_sessions.emplace_back(m_sessions.back(), nullptr);
   }
   ++m_revision;
}

template <typename Session>
void undo_stack<Session>::squash() {
   if (m_sessions.empty()) {
      return;
   }
   m_sessions.back().commit();
   m_sessions.back().detach();
   m_sessions.pop_back();
   --m_revision;
}

template <typename Session>
void undo_stack<Session>::undo() {
   if (m_sessions.empty()) {
      return;
   }
   m_sessions.back().detach();
   m_sessions.pop_back();
   --m_revision;
}

template <typename Session>
void undo_stack<Session>::commit(int64_t revision) {
   if (m_sessions.empty()) {
      return;
   }

   revision              = std::min(revision, m_revision);
   auto initial_revision = static_cast<int64_t>(m_revision - m_sessions.size() + 1);
   if (initial_revision > revision) {
      return;
   }

   const auto start_index = revision - initial_revision;

   for (int64_t i = start_index; i >= 0; --i) { m_sessions[i].commit(); }
   m_sessions.erase(std::begin(m_sessions), std::begin(m_sessions) + start_index + 1);
   if (!m_sessions.empty()) {
      m_sessions.front().attach(*m_head);
   }
}

template <typename Session>
bool undo_stack<Session>::empty() const {
   return m_sessions.empty();
}

template <typename Session>
size_t undo_stack<Session>::size() const {
   return m_sessions.size();
}

template <typename Session>
int64_t undo_stack<Session>::revision() const {
   return m_revision;
}

template <typename Session>
void undo_stack<Session>::revision(int64_t revision) {
   if (!empty()) {
      return;
   }

   if (revision <= m_revision) {
      return;
   }

   m_revision = revision;
}

template <typename Session>
typename undo_stack<Session>::variant_type undo_stack<Session>::top() {
   if (!m_sessions.empty()) {
      auto& back = m_sessions.back();
      return { back, nullptr };
   }
   return { *m_head, nullptr };
}

template <typename Session>
typename undo_stack<Session>::const_variant_type undo_stack<Session>::top() const {
   if (!m_sessions.empty()) {
      auto& back = m_sessions.back();
      return { back, nullptr };
   }
   return { *m_head, nullptr };
}

template <typename Session>
typename undo_stack<Session>::variant_type undo_stack<Session>::bottom() {
   if (!m_sessions.empty()) {
      auto& front = m_sessions.front();
      return { front, nullptr };
   }
   return { *m_head, nullptr };
}

template <typename Session>
typename undo_stack<Session>::const_variant_type undo_stack<Session>::bottom() const {
   if (!m_sessions.empty()) {
      auto& front = m_sessions.front();
      return { front, nullptr };
   }
   return { *m_head, nullptr };
}

template <typename Session>
void undo_stack<Session>::open() {
   if (m_datadir.empty())
      return;

   if (!fc::is_directory(m_datadir))
      fc::create_directories(m_datadir);

   auto undo_stack_dat = m_datadir / undo_stack_filename;
   if( fc::exists( undo_stack_dat ) ) {
      try {
         std::string content;
         fc::read_file_contents( undo_stack_dat, content );

         fc::datastream<const char*> ds( content.data(), content.size() );

         // validate totem
         uint32_t totem = 0;
         fc::raw::unpack( ds, totem );
         EOS_ASSERT( totem == undo_stack_magic_number, eosio::chain::chain_exception,
                     "Undo stack data file '${filename}' has unexpected magic number: ${actual_totem}. Expected ${expected_totem}",
                     ("filename", undo_stack_dat.generic_string())
                     ("actual_totem", totem)
                     ("expected_totem", undo_stack_magic_number)
         );

         // validate version
         uint32_t version = 0;
         fc::raw::unpack( ds, version );
         EOS_ASSERT( version >= undo_stack_min_supported_version && version <= undo_stack_max_supported_version,
                    eosio::chain::chain_exception,
                    "Unsupported version of Undo stack data file '${filename}'. "
                    "Undo stack data version is ${version} while code supports version(s) [${min},${max}]",
                    ("filename", undo_stack_dat.generic_string())
                    ("version", version)
                    ("min", undo_stack_min_supported_version)
                    ("max", undo_stack_max_supported_version)
         );

         int64_t rev; fc::raw::unpack( ds, rev );
         revision(rev);

         size_t num_sessions; fc::raw::unpack( ds, num_sessions );
         for( size_t i = 0; i < num_sessions; ++i ) {
            session_type session;
            if (m_sessions.empty())
               session.attach(*m_head);
            else
               session.attach(m_sessions.back());

            size_t num_updated_keys; fc::raw::unpack( ds, num_updated_keys );
            for( size_t j = 0; j < num_updated_keys; ++j ) {
               shared_bytes_reflect raw_key; fc::raw::unpack( ds, raw_key );
               shared_bytes_reflect raw_value; fc::raw::unpack( ds, raw_value );

               shared_bytes key(raw_key.data.data(), raw_key.data.size());
               shared_bytes value(raw_value.data.data(), raw_value.data.size());
               session.write(key, value);
            }

            size_t num_deleted_keys; fc::raw::unpack( ds, num_deleted_keys );
            for( size_t j = 0; j < num_deleted_keys; ++j ) {
               shared_bytes_reflect raw_key; fc::raw::unpack( ds, raw_key );

               shared_bytes key(raw_key.data.data(), raw_key.data.size());
               session.erase(key);
            }

            m_sessions.emplace_back(std::move(session));
         }
      } FC_CAPTURE_AND_RETHROW( (undo_stack_dat) )

      fc::remove( undo_stack_dat );
   }
}

template <typename Session>
void undo_stack<Session>::close() {
   if (m_datadir.empty())
      return;

   auto undo_stack_dat = m_datadir / undo_stack_filename;

   std::ofstream out( undo_stack_dat.generic_string().c_str(), std::ios::out | std::ios::binary | std::ofstream::trunc );
   fc::raw::pack( out, undo_stack_magic_number );
   fc::raw::pack( out, undo_stack_max_supported_version );

   fc::raw::pack( out, revision() );
   fc::raw::pack( out, size() ); // number of sessions

   while ( !m_sessions.empty() ) {
      auto& session = m_sessions.front();
      auto updated_keys = session.updated_keys();
      fc::raw::pack( out, updated_keys.size() ); // number of updated keys

      for (const auto& key: updated_keys) {
        auto value = session.read(key);

        if ( value ) {
           shared_bytes_reflect raw_key; 
           raw_key.data.assign(key.data(), key.data() + key.size());
           shared_bytes_reflect raw_value;
           raw_value.data.assign(value->data(), value->data() + value->size());
           fc::raw::pack( out, raw_key );
           fc::raw::pack( out, raw_value );
        }
      }

      auto deleted_keys = session.deleted_keys();
      fc::raw::pack( out, deleted_keys.size() ); // number of deleted keys

      for (const auto& key: deleted_keys) {
         fc::raw::pack( out, key );
      }

      session.detach();
      m_sessions.pop_front();
   }
}
} // namespace eosio::session
