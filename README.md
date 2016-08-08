# lru_map: Policy based LRU Cache Map

This library provides a generic LRU (Least Recently Used) Map with
cost free client-specifiable policies.

The LRU class template, LruMap, maintains a mapping from (generic) KeyType
to (generic) ValueType. It maintains only the most recently used
'capacity' elements where capacity is specified as a constructor argument.
When the map is full, a request to insert a new element discards the
least recently used element.

The basic design consists of a map and a list. The map maintains the
key to value mappings. The list maintains the order or usage, the most
recent one is at front, and the least recent one is at back. There are
three basic APIs: (1) Insert, (2) Find, and (3) Erase. When a request
to Find() is successful, the element is considered used and moved to
the front (most recent or hot) of the list. Similarly for Insert() when
the key was already existing.

Beyond the basics, there are couple of possibilities that appear into
the design space, although not all of them may be applicable to a single
client. Examples include:

 1. Is the class thread-safe?

    Thread safety has certain cost. Some clients may be willing to pay
    the cost while others, which are already operating in a thread safe
    context, may not.

 2. Can we maintain the timestamps when an element was inserted and
    last accessed?

 3. Can we maintain a counter to count how many times an element is accessed?

 4. Can we log when some or all of the interesting events among
    Insert|Overflow|Find|Erase occur?

All these are policies -- independent policies -- that a client may want.

One way to address this is to roll out an Options struct with one or more
variables for each of the policies. A client constructs an object of that
struct and populates the member variables suitable to her needs. The
object is then passed as a constructor argument to the main class
(here, LruMap) where it is saved as a (const) member variable.
Then on, the class chooses its behavior in accordance to passed options.

Another way to address this is to introduce some configuration mechanism
(for example, gflags) corresponding to each option.

Athough these ways work quite well, they are in friction with
C++'s "Zero Overhead Principle", a principle outlined in
"The C++ Programming Language, 4th Edition". The principle is also known
as "You don't pay for what you don't need".

Why so?

 - Because member variables may need to be maintained even when they
   are not needed by all clients.

 - Because run time checks may need to be performed even when they
   are not needed by all clients.

This design of this class explores an alternate route. It allows the
clients to specify the policies with compile time template parameters.

Thus, the LruMap class template accepts template parameters for each
of the independent policies mentioned above:
 - Locking (thread-safety) (Note 0)
 - Timestamps (element access time, modify time)
 - HitCounter (element access counter)
 - Logging (logging API calls)

The default behavior of LruMap is to choose the default behavior for
each of the policies. The respective policy classes offering the
default behavior is provided at the end of this file. In general, the
default policy classes choose the empty/none/least-cost alternative.
While instantiating LruMap, a client may throw in a policy class of
her own (Note 1). In addition to the default policies, this file
also provides couple of non-default policies. As an example, the
unit test class instantiates different combinations of those.

# How to build?

  mkdir build
  cd build
  cmake ..
  make
  ./test/lru_map_test

## How to use clang++?
  export CXX=/usr/bin/clang++
  cmake ..

## How to view compilation and linking commands?
  make VERBOSE=1

## How to clean?
  cd build
  rm ./* -rf

