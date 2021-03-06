#include "ramrbtree.h"
#include <stdlib.h>
#include <regex>
#include <set>

#define SINGLE_THREADED

using namespace Component;
using namespace std;

static set<string> m_index;

RamRBTree::RamRBTree(const std::string& owner, const std::string& name) {}

RamRBTree::RamRBTree() {}

RamRBTree::~RamRBTree() {}

void RamRBTree::insert(const string& key)
{
  if (!m_index.insert(key).second) {
    throw(API_exception("insert index failed"));
  }
}

void RamRBTree::erase(const std::string& key) { m_index.erase(key); }

void RamRBTree::clear() { m_index.clear(); }

string RamRBTree::get(offset_t position) const
{
  if (position >= m_index.size()) {
    throw out_of_range("Position out of range");
  }

  auto it = m_index.begin();
  advance(it, position);
  return *it;
}

size_t RamRBTree::count() const { return m_index.size(); }

string RamRBTree::find(const std::string& key_expression,
                       offset_t           begin_position,
                       find_t             find_type,
                       offset_t&          out_end_position)
{
  std::regex r(key_expression);
  if (begin_position >= m_index.size()) {
    throw out_of_range("Position out of range");
  }

  if (out_end_position >= m_index.size()) {
    throw out_of_range("Position out of range");
  }

  switch (find_type) {
    case FIND_TYPE_REGEX:
      for (int i = begin_position; i <= out_end_position; i++) {
        string key = RamRBTree::get(i);
        if (regex_match(key, r)) {
          return key;
        }
      }
      break;
    case FIND_TYPE_EXACT:
      for (int i = begin_position; i <= out_end_position; i++) {
        string key = RamRBTree::get(i);
        if (key.compare(key_expression) == 0) {
          return key;
        }
      }
      break;
    case FIND_TYPE_PREFIX:
      for (int i = begin_position; i <= out_end_position; i++) {
        string key = RamRBTree::get(i);
        if (key.find(key_expression) != string::npos) {
          return key;
        }
      }
      break;
    case FIND_TYPE_NEXT:
      string key = get(begin_position + 1);
      return key;
      break;
  }

  return "";
}

/**
 * Factory entry point
 *
 */
extern "C" void* factory_createInstance(Component::uuid_t& component_id)
{
  if (component_id == RamRBTree_factory::component_id()) {
    return static_cast<void*>(new RamRBTree_factory());
  }
  else
    return NULL;
}
