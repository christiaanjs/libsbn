// Copyright 2019 Matsen group.
// libsbn is free software under the GPLv3; see LICENSE file for details.

#include "node.hpp"
#include <limits.h>
#include <algorithm>
#include <cassert>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

Node::Node(uint32_t leaf_id)
    : children_({}),
      index_(leaf_id),
      tag_(PackInts(leaf_id, 1)),
      hash_(SOHash(leaf_id)) {}

Node::Node(NodePtrVec children, size_t index)
    : children_(children), index_(index) {
  if (children_.empty()) {
    std::cerr << "Called internal node constructor with no children.\n";
    abort();
  }
  // Order the children by their max leaf ids.
  std::sort(children_.begin(), children_.end(),
            [](const auto& lhs, const auto& rhs) {
              if (lhs->MaxLeafID() == rhs->MaxLeafID()) {
                // Children should have non-overlapping leaf sets, so there
                // should not be ties.
                std::cout << "Tie observed between " << lhs->Newick() << " and "
                          << rhs->Newick() << std::endl;
                std::cout << "Do you have a taxon name repeated?\n";
                abort();
              }
              return (lhs->MaxLeafID() < rhs->MaxLeafID());
            });
  // Children are sorted by their max_leaf_id, so we can get the max by
  // looking at the last element.
  uint32_t max_leaf_id = children_.back()->MaxLeafID();
  uint32_t leaf_count = 0;
  hash_ = 0;
  for (const auto& child : children_) {
    leaf_count += child->LeafCount();
    hash_ ^= child->Hash();
  }
  tag_ = PackInts(max_leaf_id, leaf_count);
  // Bit rotation is necessary because if we only XOR then we can get
  // collisions when identical tips are in different
  // ordered subtrees (an example is in below doctest).
  hash_ = SORotate(hash_, 1);
}

bool Node::operator==(const Node& other) {
  if (this->Hash() != other.Hash()) {
    return false;
  }
  size_t child_count = this->Children().size();
  if (child_count != other.Children().size()) {
    return false;
  }
  for (size_t i = 0; i < child_count; i++) {
    if (!(*children_[i] == *other.Children()[i])) {
      return false;
    }
  }
  return true;
}

void Node::PreOrder(std::function<void(const Node*)> f) const {
  f(this);
  for (const auto& child : children_) {
    child->PreOrder(f);
  }
}

// Iterate f through (parent, sister, node) for internal nodes using a
// preorder traversal.
void Node::TriplePreOrderInternal(
    std::function<void(const Node*, const Node*, const Node*)> f) const {
  if (!IsLeaf()) {
    assert(children_.size() == 2);
    f(this, children_[1].get(), children_[0].get());
    children_[0]->TriplePreOrderInternal(f);
    f(this, children_[0].get(), children_[1].get());
    children_[1]->TriplePreOrderInternal(f);
  }
}

// Traversal for rooted pairs in an unrooted subtree in its traditional rooted
// representation.
// We take in two functions, f_root, and f_internal, each of which take three
// edges.
// We assume that f_root is symmetric in its last two arguments so that
// f_root's signature actually looks like f_root(node0, {node1, node2}).
// We apply f_root to the descendant edges like so: 012, 120, and 201. Because
// f_root is symmetric in the last two arguments, we are going to get all of
// the distinct calls of f.
// At the internal nodes we cycle through triples of (parent, sister, node)
// for f_internal.
void Node::TriplePreOrder(
    std::function<void(const Node*, const Node*, const Node*)> f_root,
    std::function<void(const Node*, const Node*, const Node*)> f_internal)
    const {
  assert(children_.size() == 3);
  f_root(children_[0].get(), children_[1].get(), children_[2].get());
  f_root(children_[1].get(), children_[2].get(), children_[0].get());
  f_root(children_[2].get(), children_[0].get(), children_[1].get());
  for (const auto& child : children_) {
    child->TriplePreOrderInternal(f_internal);
  }
}

// See the typedef of PCSSFun to understand the argument type to this
// function.
void Node::PCSSPreOrder(PCSSFun f) const {
  this->TriplePreOrder(
      // f_root
      [&f](const Node* node0, const Node* node1, const Node* node2) {
        // Virtual root on node2's edge, with subsplit pointing up.
        f(node2, false, node2, true, node0, false, node1, false);
        if (!node2->IsLeaf()) {
          assert(node2->Children().size() == 2);
          auto child0 = node2->Children()[0].get();
          auto child1 = node2->Children()[1].get();
          // Virtual root in node1.
          f(node0, false, node2, false, child0, false, child1, false);
          // Virtual root in node0.
          f(node1, false, node2, false, child0, false, child1, false);
          // Virtual root on node2's edge, with subsplit pointing down.
          f(node2, true, node2, false, child0, false, child1, false);
          // Virtual root in child0.
          f(child1, false, node2, true, node0, false, node1, false);
          // Virtual root in child1.
          f(child0, false, node2, true, node0, false, node1, false);
        }
      },
      // f_internal
      [&f](const Node* parent, const Node* sister, const Node* node) {
        // Virtual root on node's edge, with subsplit pointing up.
        f(node, false, node, true, parent, true, sister, false);
        if (!node->IsLeaf()) {
          assert(node->Children().size() == 2);
          auto child0 = node->Children()[0].get();
          auto child1 = node->Children()[1].get();
          // Virtual root up the tree.
          f(sister, false, node, false, child0, false, child1, false);
          // Virtual root in sister.
          f(parent, true, node, false, child0, false, child1, false);
          // Virtual root on node's edge, with subsplit pointing down.
          f(node, true, node, false, child0, false, child1, false);
          // Virtual root in child0.
          f(child1, false, node, true, sister, false, parent, true);
          // Virtual root in child1.
          f(child0, false, node, true, sister, false, parent, true);
        }
      });
}

void Node::PostOrder(std::function<void(const Node*)> f) const {
  for (const auto& child : children_) {
    child->PostOrder(f);
  }
  f(this);
}

void Node::LevelOrder(std::function<void(const Node*)> f) const {
  std::deque<const Node*> to_visit = {this};
  while (to_visit.size()) {
    auto n = to_visit.front();
    f(n);
    to_visit.pop_front();

    for (const auto& child : n->children_) {
      to_visit.push_back(child.get());
    }
  }
}

// This function assigns indices to the nodes of the topology: the leaves get
// their indices (which are contiguously numbered from 0 through the leaf
// count -1) and the rest get ordered according to a postorder traversal. Thus
// the root always has index equal to the number of nodes in the tree.
//
// This function returns a map that maps the tags to their indices.
TagSizeMap Node::Reindex() {
  TagSizeMap tag_index_map;
  size_t next_index = 1 + MaxLeafID();
  MutablePostOrder([&tag_index_map, &next_index](Node* node) {
    if (node->IsLeaf()) {
      node->index_ = node->MaxLeafID();
    } else {
      node->index_ = next_index;
      next_index++;
    }
    assert(tag_index_map.insert({node->Tag(), node->index_}).second);
  });
  return tag_index_map;
}

std::string Node::Newick(const DoubleVectorOption& branch_lengths,
                         const TagStringMapOption& node_labels) const {
  return NewickAux(branch_lengths, node_labels) + ";";
}

std::string Node::NewickAux(const DoubleVectorOption& branch_lengths,
                            const TagStringMapOption& node_labels) const {
  std::string str;
  if (IsLeaf()) {
    if (node_labels) {
      str.assign((*node_labels).at(Tag()));
    } else {
      str.assign(TagString());
    }
  } else {
    str.assign("(");
    for (auto iter = children_.begin(); iter != children_.end(); iter++) {
      if (iter != children_.begin()) {
        str.append(",");
      }
      str.append((*iter)->NewickAux(branch_lengths, node_labels));
    }
    str.append(")");
    if (!node_labels) {
      // If node_labels are not included then we figure that the discrete
      // structure of the tree is of interest. Thus we write out the tags as
      // internal nodes of the tree.
      str.append(TagString());
    }
  }
  if (branch_lengths) {
    assert(Index() < (*branch_lengths).size());
    // ostringstream is the way to get scientific notation using the STL.
    std::ostringstream str_stream;
    str_stream << (*branch_lengths)[Index()];
    str.append(":" + str_stream.str());
  }
  return str;
}

// Class methods
Node::NodePtr Node::Leaf(uint32_t id) { return std::make_shared<Node>(id); }
Node::NodePtr Node::Join(NodePtrVec children, size_t index) {
  return std::make_shared<Node>(children, index);
}
Node::NodePtr Node::Join(NodePtr left, NodePtr right, size_t index) {
  return Join(std::vector<NodePtr>({left, right}), index);
}

Node::NodePtrVec Node::ExampleTopologies() {
  NodePtrVec topologies = {
      // 0: (0,1,(2,3))
      Join(std::vector<NodePtr>({Leaf(0), Leaf(1), Join(Leaf(2), Leaf(3))})),
      // 1; (0,1,(2,3)) again
      Join(std::vector<NodePtr>({Leaf(1), Leaf(0), Join(Leaf(3), Leaf(2))})),
      // 2: (0,2,(1,3))
      Join(std::vector<NodePtr>({Leaf(0), Leaf(2), Join(Leaf(1), Leaf(3))})),
      // 3: (0,(1,(2,3)))
      Join(std::vector<NodePtr>(
          {Leaf(0), Join(Leaf(1), Join(Leaf(2), Leaf(3)))}))};
  for (auto& topology : topologies) {
    topology->Reindex();
  }
  return topologies;
}

inline uint32_t Node::SOHash(uint32_t x) {
  x = ((x >> 16) ^ x) * 0x45d9f3b;
  x = ((x >> 16) ^ x) * 0x45d9f3b;
  x = (x >> 16) ^ x;
  return x;
}

inline size_t Node::SORotate(size_t n, uint32_t c) {
  const uint32_t mask =
      (CHAR_BIT * sizeof(n) - 1);  // assumes width is a power of 2.
  // assert ( (c<=mask) &&"rotate by type width or more");
  c &= mask;
  return (n << c) | (n >> ((-c) & mask));
}

void Node::MutablePostOrder(std::function<void(Node*)> f) {
  for (const auto& child : children_) {
    child->MutablePostOrder(f);
  }
  f(this);
}

#ifdef DOCTEST_LIBRARY_INCLUDED
TEST_CASE("Node header") {
  Node::NodePtrVec examples = Node::ExampleTopologies();
  Node::NodePtr t1 = examples[0];
  Node::NodePtr t1_twin = examples[1];
  Node::NodePtr t2 = examples[2];
  Node::NodePtr t3 = examples[3];
  // TODO(ematsen) add real test for TriplePreorder
  std::cout << "TriplePreOrder" << std::endl;
  std::cout << t2->Newick() << std::endl;
  auto print_triple = [](const Node* parent, const Node* sister,
                         const Node* node) {
    std::cout << parent->TagString() << ", " << sister->TagString() << ", "
              << node->TagString() << " " << std::endl;
  };
  t2->TriplePreOrder(print_triple, print_triple);

  // This is actually a non-trivial test (see note in Node constructor above),
  // which shows why we need bit rotation.
  CHECK_NE(t1->Hash(), t2->Hash());

  CHECK_EQ(t1, t1_twin);
  CHECK_NE(t1, t2);
}
#endif  // DOCTEST_LIBRARY_INCLUDED
