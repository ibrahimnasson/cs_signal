/***********************************************************************
*
* Copyright (c) 2015-2019 Barbara Geller
* Copyright (c) 2015-2019 Ansel Sermersheim
*
* This file is part of libguarded.
*
* libguarded is free software, released under the BSD 2-Clause license.
* For license details refer to LICENSE provided with this project.
*
* CopperSpice is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*
* https://opensource.org/licenses/BSD-2-Clause
*
***********************************************************************/

#ifndef LIBGUARDED_RCU_LIST_HPP
#define LIBGUARDED_RCU_LIST_HPP

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>

#include "rcu_guarded.hpp"

namespace libguarded
{

/**
   \headerfile rcu_list.hpp <libguarded/rcu_list.hpp>

   This templated class implements a linked list which is maintained
   using the RCU algorithm. Only one thread at a time may modify the
   linked list, but any number of threads may read
   simultaneously. Ongoing writes will not block readers. As a reader
   traverses the list while mutating operations are ongoing, the
   reader may see the old state or the new state.

   Since the RCU algorithm does not reap nodes until all readers who
   could have seen the node have completed, iterators are never
   invalidated by any list operation.

   This class will use std::mutex for the internal locking mechanism
   by default. Other classes which are useful for the mutex type are
   std::recursive_mutex, std::timed_mutex, and
   std::recursive_timed_mutex.
*/
template <typename T, typename M = std::mutex, typename Alloc = std::allocator<T>>
class rcu_list
{
   public:
      using value_type      = T;
      using allocator_type  = Alloc;
      using size_type       = std::ptrdiff_t;
      using reference       = value_type &;
      using const_reference = const value_type &;
      using pointer         = typename std::allocator_traits<Alloc>::pointer;
      using const_pointer   = typename std::allocator_traits<Alloc>::const_pointer;

      class iterator;
      class const_iterator;
      class reverse_iterator;
      class const_reverse_iterator;
      class end_iterator;
      class end_reverse_iterator;

      class rcu_guard;
      using rcu_write_guard = rcu_guard;
      using rcu_read_guard  = rcu_guard;

      rcu_list();
      explicit rcu_list(const Alloc &alloc);

      rcu_list(const rcu_list &) = delete;
      rcu_list(rcu_list &&) = delete;
      rcu_list &operator=(const rcu_list &) = delete;
      rcu_list &operator=(rcu_list &&) = delete;

      ~rcu_list();

      iterator begin();
      end_iterator end();
      const_iterator begin() const;
      end_iterator end() const;
      const_iterator cbegin() const;
      end_iterator cend() const;

      void clear();

      iterator insert(const_iterator pos, T value);
      iterator insert(const_iterator pos, size_type count, const T &value);

      template <typename InputIter>
      iterator insert(const_iterator pos, InputIter first, InputIter last);
      iterator insert(const_iterator pos, std::initializer_list<T> ilist);

      template <typename... Us>
      iterator emplace(const_iterator pos, Us &&... vs);

      void push_front(T value);
      void push_back(T value);

      template <typename... Us>
      void emplace_front(Us &&... vs);

      template <typename... Us>
      void emplace_back(Us &&... vs);

      iterator erase(const_iterator pos);

   private:
      struct node {
         // uncopyable, unmoveable
         node(const node &) = delete;
         node(node &&) = delete;
         node &operator=(const node &) = delete;
         node &operator=(node &&) = delete;

         template <typename... Us>
         explicit node(Us &&... vs)
            : data(std::forward<Us>(vs)...)
         {
         }

         std::atomic<node *> next{nullptr};
         std::atomic<node *> back{nullptr};
         bool deleted{false};
         T data;
      };

   struct zombie_list_node {
      zombie_list_node(node *n) noexcept : zombie_node(n)
      {
      }

      zombie_list_node(rcu_guard *g) noexcept : owner(g)
      {
      }

      // uncopyable, unmoveable
      zombie_list_node(const zombie_list_node &) = delete;
      zombie_list_node(zombie_list_node &&) = delete;
      zombie_list_node &operator=(const zombie_list_node &) = delete;
      zombie_list_node &operator=(zombie_list_node &&) = delete;

      std::atomic<zombie_list_node *> next{nullptr};
      std::atomic<rcu_guard *> owner{nullptr};
      node *zombie_node{nullptr};
   };

   using alloc_trait        = std::allocator_traits<Alloc>;
   using node_alloc_t       = typename alloc_trait::template rebind_alloc<node>;
   using node_alloc_trait   = std::allocator_traits<node_alloc_t>;
   using zombie_alloc_t     = typename alloc_trait::template rebind_alloc<zombie_list_node>;
   using zombie_alloc_trait = std::allocator_traits<zombie_alloc_t>;

   std::atomic<node *> m_head{nullptr};
   std::atomic<node *> m_tail{nullptr};

   mutable std::atomic<zombie_list_node *> m_zombie_head{nullptr};

   M m_write_mutex;

   mutable node_alloc_t m_node_alloc;
   mutable zombie_alloc_t m_zombie_alloc;
};

/*----------------------------------------*/

namespace detail
{

// allocator-aware deleter for unique_ptr
template <typename Alloc>
class deallocator
{
   using allocator_type   = Alloc;
   using allocator_traits = std::allocator_traits<allocator_type>;
   using pointer          = typename allocator_traits::pointer;

   allocator_type alloc;

   public:
   explicit deallocator(const allocator_type &alloc) noexcept : alloc(alloc)
   {
   }

   void operator()(pointer p)
   {
      if (p != nullptr) {
         allocator_traits::destroy(alloc, p);
         allocator_traits::deallocate(alloc, p, 1);
      }
   }
};

// unique_ptr counterpart for std::allocate_shared()
template <typename T, typename Alloc, typename... Args>
std::unique_ptr<T, deallocator<Alloc>> allocate_unique(Alloc &alloc, Args &&... args)
{
   using allocator_traits = std::allocator_traits<Alloc>;
   auto p = allocator_traits::allocate(alloc, 1);

   try {
      allocator_traits::construct(alloc, p, std::forward<Args>(args)...);
      return {p, deallocator<Alloc>{alloc}};
   } catch (...) {
      allocator_traits::deallocate(alloc, p, 1);
      throw;
   }
}

} // namespace detail

/*----------------------------------------*/

template <typename T, typename M, typename Alloc>
class rcu_list<T, M, Alloc>::rcu_guard
{
   public:
      void rcu_read_lock(const rcu_list<T, M, Alloc> &list);
      void rcu_read_unlock(const rcu_list<T, M, Alloc> &list);
      void rcu_write_lock(const rcu_list<T, M, Alloc> &list);
      void rcu_write_unlock(const rcu_list<T, M, Alloc> &list);

   private:
      void unlock();

     zombie_list_node *m_zombie;
     const rcu_list<T, M, Alloc> *m_list;
};

template <typename T, typename M, typename Alloc>
void rcu_list<T, M, Alloc>::rcu_guard::rcu_read_lock(const rcu_list<T, M, Alloc> &list)
{
   m_list   = &list;
   m_zombie = zombie_alloc_trait::allocate(list.m_zombie_alloc, 1);
   zombie_alloc_trait::construct(list.m_zombie_alloc, m_zombie, this);
   zombie_list_node *oldNext = list.m_zombie_head.load(std::memory_order_relaxed);

   do {
      m_zombie->next.store(oldNext, std::memory_order_relaxed);
   } while (!list.m_zombie_head.compare_exchange_weak(oldNext, m_zombie));
}

template <typename T, typename M, typename Alloc>
void rcu_list<T, M, Alloc>::rcu_guard::rcu_read_unlock(const rcu_list<T, M, Alloc> &)
{
   unlock();
};

template <typename T, typename M, typename Alloc>
void rcu_list<T, M, Alloc>::rcu_guard::unlock()
{
   zombie_list_node *cached_next = m_zombie->next.load();
   zombie_list_node *n           = cached_next;

   bool last = true;

   while (n) {
      if (n->owner.load() != nullptr) {
         last = false;
         break;
      }

      n = n->next.load();
   }

   n = cached_next;

   if (last) {
      while (n) {
         node *deadNode = n->zombie_node;

         if (deadNode != nullptr) {
            node_alloc_trait::destroy(m_list->m_node_alloc, deadNode);
            node_alloc_trait::deallocate(m_list->m_node_alloc, deadNode, 1);
         }

         zombie_list_node *oldnode = n;
         n = n->next.load();

         if (oldnode != nullptr) {
            zombie_alloc_trait::destroy(m_list->m_zombie_alloc, oldnode);
            zombie_alloc_trait::deallocate(m_list->m_zombie_alloc, oldnode, 1);
         }
      }

      m_zombie->next.store(n);
   }

   m_zombie->owner.store(nullptr);
}

template <typename T, typename M, typename Alloc>
void rcu_list<T, M, Alloc>::rcu_guard::rcu_write_lock(const rcu_list<T, M, Alloc> &list)
{
   rcu_read_lock(list);
}

template <typename T, typename M, typename Alloc>
void rcu_list<T, M, Alloc>::rcu_guard::rcu_write_unlock(const rcu_list<T, M, Alloc> &list)
{
   rcu_read_unlock(list);
}

/*----------------------------------------*/

template <typename T, typename M, typename Alloc>
class rcu_list<T, M, Alloc>::iterator
{
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = const T;
    using pointer           = const T *;
    using reference         = const T &;
    using difference_type   = size_t;

    iterator() : m_current(nullptr)
    {
    }

    const T &operator*() const
    {
        return m_current->data;
    }

    const T *operator->() const
    {
        return &(m_current->data);
    }

    bool operator==(const end_iterator &) const
    {
        return m_current == nullptr;
    }

    bool operator!=(const end_iterator &) const
    {
        return m_current != nullptr;
    }

    iterator &operator++()
    {
        m_current = m_current->next.load();
        return *this;
    }

    iterator &operator--()
    {
        m_current = m_current->prev.load();
        return *this;
    }

    iterator operator++(int)
    {
        iterator old(*this);
        ++(*this);
        return old;
    }

    iterator operator--(int)
    {
       iterator old(*this);
        --(*this);
        return old;
    }

  private:
    friend rcu_list<T, M, Alloc>;
    friend rcu_list<T, M, Alloc>::const_iterator;

    explicit iterator(const typename rcu_list<T, M, Alloc>::const_iterator &it)
       : m_current(it.m_current)
    {
    }

    explicit iterator(node *n) : m_current(n)
    {
    }

    node *m_current;
};

/*----------------------------------------*/

template <typename T, typename M, typename Alloc>
class rcu_list<T, M, Alloc>::const_iterator
{
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = const T;
    using pointer           = const T *;
    using reference         = const T &;
    using difference_type   = size_t;

    const_iterator() : m_current(nullptr){};
    const_iterator(const typename rcu_list<T, M, Alloc>::iterator &it) : m_current(it.m_current){};

   const T &operator*() const
    {
        return m_current->data;
    }

    const T *operator->() const
    {
        return &(m_current->data);
    }

    bool operator==(const end_iterator &) const
    {
        return m_current == nullptr;
    }

    bool operator!=(const end_iterator &) const
    {
        return m_current != nullptr;
    }

    const_iterator &operator++()
    {
        m_current = m_current->next.load();
        return *this;
    }

    const_iterator &operator--()
    {
        m_current = m_current->prev.load();
        return *this;
    }

    const_iterator operator++(int)
    {
        const_iterator old(*this);
        ++(*this);
        return old;
    }

    const_iterator operator--(int)
    {
        const_iterator old(*this);
        --(*this);
        return old;
    }

  private:
    friend rcu_list<T, M, Alloc>;

    explicit const_iterator(node *n)
       : m_current(n)
    {
    }

    node *m_current;
};

/*----------------------------------------*/

template <typename T, typename M, typename Alloc>
class rcu_list<T, M, Alloc>::end_iterator
{
  public:
    bool operator==(iterator iter) const
    {
        return iter == *this;
    }

    bool operator!=(iterator iter) const
    {
        return iter != *this;
    }

    bool operator==(const_iterator iter) const
    {
        return iter == *this;
    }

    bool operator!=(const_iterator iter) const
    {
        return iter != *this;
    }
};

/*----------------------------------------*/

template <typename T, typename M, typename Alloc>
rcu_list<T, M, Alloc>::rcu_list()
{
   m_head.store(nullptr);
   m_tail.store(nullptr);
}

template <typename T, typename M, typename Alloc>
rcu_list<T, M, Alloc>::rcu_list(const Alloc &alloc) : m_node_alloc(alloc), m_zombie_alloc(alloc)
{
}

template <typename T, typename M, typename Alloc>
rcu_list<T, M, Alloc>::~rcu_list()
{
   node *n = m_head.load();

   while (n != nullptr) {
      node *current = n;
      n = n->next.load();

      if (current != nullptr) {
         node_alloc_trait::destroy(m_node_alloc, current);
         node_alloc_trait::deallocate(m_node_alloc, current, 1);
      }
   }

   zombie_list_node *zn = m_zombie_head.load();

   while (zn != nullptr && zn->owner.load() == nullptr) {
      zombie_list_node *current = zn;
      zn = zn->next.load();

      if (current->zombie_node != nullptr) {
         node_alloc_trait::destroy(m_node_alloc, current->zombie_node);
         node_alloc_trait::deallocate(m_node_alloc, current->zombie_node, 1);
      }

      if (current != nullptr) {
         zombie_alloc_trait::destroy(m_zombie_alloc, current);
         zombie_alloc_trait::deallocate(m_zombie_alloc, current, 1);
      }
   }
}

template <typename T, typename M, typename Alloc>
auto rcu_list<T, M, Alloc>::begin() -> iterator
{
   return iterator(m_head.load());
}

template <typename T, typename M, typename Alloc>
auto rcu_list<T, M, Alloc>::end() -> end_iterator
{
   return end_iterator();
}

template <typename T, typename M, typename Alloc>
auto rcu_list<T, M, Alloc>::begin() const -> const_iterator
{
   return const_iterator(m_head.load());
}

template <typename T, typename M, typename Alloc>
auto rcu_list<T, M, Alloc>::end() const -> end_iterator
{
   return end_iterator();
}

template <typename T, typename M, typename Alloc>
void rcu_list<T, M, Alloc>::push_front(T data)
{
   std::lock_guard<M> guard(m_write_mutex);
   auto newNode = detail::allocate_unique<node>(m_node_alloc, std::move(data));

   node *oldHead = m_head.load();

   if (oldHead == nullptr) {
      m_head.store(newNode.get());
      m_tail.store(newNode.release());
   } else {
      newNode->next.store(oldHead);
      oldHead->back.store(newNode.get());
      m_head.store(newNode.release());
   }
}

template <typename T, typename M, typename Alloc>
template <typename... Us>
void rcu_list<T, M, Alloc>::emplace_front(Us &&... vs)
{
   std::lock_guard<M> guard(m_write_mutex);
   auto newNode = detail::allocate_unique<node>(m_node_alloc, std::forward<Us>(vs)...);

   node *oldHead = m_head.load();

   if (oldHead == nullptr) {
      m_head.store(newNode.get());
      m_tail.store(newNode.release());
   } else {
      newNode->next.store(oldHead);
      oldHead->back.store(newNode.get());
      m_head.store(newNode.release());
   }
}

template <typename T, typename M, typename Alloc>
void rcu_list<T, M, Alloc>::push_back(T data)
{
   std::lock_guard<M> guard(m_write_mutex);
   auto newNode = detail::allocate_unique<node>(m_node_alloc, std::move(data));

   node *oldTail = m_tail.load(std::memory_order_relaxed);

   if (oldTail == nullptr) {
      m_head.store(newNode.get());
      m_tail.store(newNode.release());
   } else {
      newNode->back.store(oldTail);
      oldTail->next.store(newNode.get());
      m_tail.store(newNode.release());
   }
}

template <typename T, typename M, typename Alloc>
template <typename... Us>
void rcu_list<T, M, Alloc>::emplace_back(Us &&... vs)
{
   std::lock_guard<M> guard(m_write_mutex);
   auto newNode = detail::allocate_unique<node>(m_node_alloc, std::forward<Us>(vs)...);

   node *oldTail = m_tail.load(std::memory_order_relaxed);

   if (oldTail == nullptr) {
      m_head.store(newNode.get());
      m_tail.store(newNode.release());
   } else {
      newNode->back.store(oldTail);
      oldTail->next.store(newNode.get());
      m_tail.store(newNode.release());
   }
}

template <typename T, typename M, typename Alloc>
auto rcu_list<T, M, Alloc>::erase(const_iterator iter) -> iterator
{
   std::lock_guard<M> guard(m_write_mutex);

   // make sure the node has not already been marked for deletion
   node *oldNext = iter.m_current->next.load();

   if (!iter.m_current->deleted) {
      iter.m_current->deleted = true;

      node *oldPrev = iter.m_current->back.load();
      node *oldNext = iter.m_current->next.load();

      if (oldPrev) {
         oldPrev->next.store(oldNext);
      } else {
         // no previous node, this node was the head
         m_head.store(oldNext);
      }

      if (oldNext) {
         oldNext->back.store(oldPrev);
      } else {
         // no next node, this node was the tail
         m_tail.store(oldPrev);
      }

      auto newZombie = zombie_alloc_trait::allocate(m_zombie_alloc, 1);
      zombie_alloc_trait::construct(m_zombie_alloc, newZombie, iter.m_current);

      zombie_list_node *oldZombie = m_zombie_head.load();

      do {
         newZombie->next = oldZombie;
      } while (!m_zombie_head.compare_exchange_weak(oldZombie, newZombie));
   }

    return iterator(oldNext);
}

template <typename T>
using SharedList = rcu_guarded<rcu_list<T>>;

}

namespace LibG = libguarded;

#endif
