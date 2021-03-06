/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/db/query/plan_enumerator.h"

#include <set>

#include "mongo/db/query/indexability.h"
#include "mongo/db/query/index_tag.h"

namespace mongo {

    PlanEnumerator::PlanEnumerator(MatchExpression* root, const vector<IndexEntry>* indices)
        : _root(root), _indices(indices) { }

    PlanEnumerator::~PlanEnumerator() {
        for (map<size_t, NodeSolution*>::iterator it = _memo.begin(); it != _memo.end(); ++it) {
            delete it->second;
        }
    }

    Status PlanEnumerator::init() {
        _inOrderCount = 0;
        _done = false;

        cout << "enumerator received root: " << _root->toString() << endl;

        // Fill out our memo structure from the tagged _root.
        _done = !prepMemo(_root);

        // Dump the tags.  We replace them with IndexTag instances.
        _root->resetTag();

        // cout << "root post-memo: " << _root->toString() << endl;

        cout << "memo dump:\n";
        for (size_t i = 0; i < _inOrderCount; ++i) {
            cout << "Node #" << i << ": " << _memo[i]->toString() << endl;
        }

        if (!_done) {
            // Tag with our first solution.
            tagMemo(_nodeToId[_root]);
            checkCompound("", _root);
        }

        return Status::OK();
    }

    bool PlanEnumerator::isCompound(size_t idx) {
        return (*_indices)[idx].keyPattern.nFields() > 1;
    }

    string PlanEnumerator::NodeSolution::toString() const {
        if (NULL != pred) {
            stringstream ss;
            ss << "predicate, first indices: [";
            for (size_t i = 0; i < pred->first.size(); ++i) {
                ss << pred->first[i];
                if (i < pred->first.size() - 1)
                    ss << ", ";
            }
            ss << "], notFirst indices: [";
            for (size_t i = 0; i < pred->notFirst.size(); ++i) {
                ss << pred->notFirst[i];
                if (i < pred->notFirst.size() - 1)
                    ss << ", ";
            }
            ss << "], pred: " << pred->expr->toString();
            return ss.str();
        }
        else if (NULL != andSolution) {
            stringstream ss;
            ss << "ONE OF: [";
            for (size_t i = 0; i < andSolution->subnodes.size(); ++i) {
                const vector<size_t>& sn = andSolution->subnodes[i];
                ss << "[";
                for (size_t j = 0; j < sn.size(); ++j) {
                    ss << sn[j];
                    if (j < sn.size() - 1)
                        ss << ", ";
                }
                ss << "]";
                if (i < andSolution->subnodes.size() - 1)
                    ss << ", ";
            }
            ss << "]";
            return ss.str();
        }
        else {
            verify(NULL != orSolution);
            stringstream ss;
            ss << "ALL OF: [";
            for (size_t i = 0; i < orSolution->subnodes.size(); ++i) {
                ss << " " << orSolution->subnodes[i];
            }
            ss << "]";
            return ss.str();
        }
    }

    /**
     * This is very expensive if the involved indices/predicates are numerous but
     * I suspect that's rare.  TODO: revisit on perf pass.
     *
     * TODO: We can save time by not bothering to do this if the index is multiKey.
     * TODO: Perhaps this should be done by the planner??
     */
    void PlanEnumerator::checkCompound(string prefix, MatchExpression* node) {
        if (MatchExpression::AND == node->matchType()) {
            // Step 1: Find all compound indices.
            vector<MatchExpression*> assignedCompound;
            vector<MatchExpression*> unassigned;

            for (size_t i = 0; i < node->numChildren(); ++i) {
                MatchExpression* child = node->getChild(i);
                if (Indexability::nodeCanUseIndexOnOwnField(child)) {
                    verify(NULL != _memo[_nodeToId[child]]);
                    verify(NULL != _memo[_nodeToId[child]]->pred);
                    if (NULL == child->getTag()) {
                        // Not assigned an index.
                        unassigned.push_back(child);
                    }
                    else {
                        IndexTag* childTag = static_cast<IndexTag*>(child->getTag());
                        if (isCompound(childTag->index)) {
                            assignedCompound.push_back(child);
                        }
                    }
                }
            }

            for (size_t i = 0; i < assignedCompound.size(); ++i) {
                cout << "assigned compound: " << assignedCompound[i]->toString();
            }
            for (size_t i = 0; i < unassigned.size(); ++i) {
                cout << "unassigned : " << unassigned[i]->toString();
            }

            // Step 2: Iterate over the other fields of the compound indices
            // TODO: This could be optimized a lot.
            for (size_t i = 0; i < assignedCompound.size(); ++i) {
                IndexTag* childTag = static_cast<IndexTag*>(assignedCompound[i]->getTag());

                // XXX: If we assign a compound index and it's on a multikey index, the planner may
                // not be able to use the multikey for it, and then it may create a new index scan,
                // and that new scan will be bogus.  For now don't assign, should fix planner to be
                // more resilient.  once we figure out fully in what cases array operators can use
                // compound indices, change this to deal with that.
                //
                // That is, we can only safely assign compound indices if the planner uses them as
                // compound indices whenever we assign them.
                if ((*_indices)[i].multikey) { continue; }

                const BSONObj& kp = (*_indices)[childTag->index].keyPattern;
                BSONObjIterator it(kp);
                it.next();

                size_t posInIdx = 0;
                // we know isCompound is true so this should be true.
                verify(it.more());
                while (it.more()) {
                    BSONElement kpElt = it.next();
                    ++posInIdx;
                    bool assignedField = false;
                    // Trying to pick an unassigned M.E.
                    for (size_t j = 0; j < unassigned.size(); ++j) {
                        // TODO: is this really required?  This seems really like it's just a
                        // reiteration of the tagging process.
                        if (prefix + unassigned[j]->path().toString() != kpElt.fieldName()) {
                            // We need to find a predicate over kpElt.fieldName().
                            continue;
                        }
                        // Another compound index was assigned.
                        if (NULL != unassigned[j]->getTag()) {
                            continue;
                        }
                        // Index no. childTag->index, the compound index, must be
                        // a member of the notFirst
                        NodeSolution* soln = _memo[_nodeToId[unassigned[j]]];
                        verify(NULL != soln);
                        verify(NULL != soln->pred);
                        verify(unassigned[j] == soln->pred->expr);
                        if (std::find(soln->pred->notFirst.begin(), soln->pred->notFirst.end(), childTag->index) != soln->pred->notFirst.end()) {
                            cout << "compound-ing " << kp.toString() << " with node " << unassigned[j]->toString() << endl;
                            assignedField = true;
                            cout << "setting pos to " << posInIdx << endl;
                            unassigned[j]->setTag(new IndexTag(childTag->index, posInIdx));
                            // We've picked something for this (index, field) tuple.  Don't pick anything else.
                            break;
                        }
                    }

                    // We must assign fields in compound indices contiguously.
                    if (!assignedField) {
                        cout << "Failed to assign to compound field " << kpElt.toString() << endl;
                        break;
                    }
                }
            }
        }

        if (Indexability::arrayUsesIndexOnChildren(node)) {
            if (!node->path().empty()) {
                prefix += node->path().toString() + ".";
            }
        }

        // Don't think the traversal order here matters.
        for (size_t i = 0; i < node->numChildren(); ++i) {
            checkCompound(prefix, node->getChild(i));
        }
    }

    bool PlanEnumerator::getNext(MatchExpression** tree) {
        if (_done) { return false; }
        *tree = _root->shallowClone();

        // Adds tags to internal nodes indicating whether or not they are indexed.
        tagForSort(*tree);

        // Sorts nodes by tags, grouping similar tags together.
        sortUsingTags(*tree);

        _root->resetTag();
        _done = true;
        return true;
    }

    bool PlanEnumerator::prepMemo(MatchExpression* node) {
        if (Indexability::arrayUsesIndexOnChildren(node)) {
            // TODO: Fold into logical->AND branch below?
            AndSolution* andSolution = new AndSolution();
            for (size_t i = 0; i < node->numChildren(); ++i) {
                if (prepMemo(node->getChild(i))) {
                    vector<size_t> option;
                    option.push_back(_nodeToId[node->getChild(i)]);
                    andSolution->subnodes.push_back(option);
                }
            }

            size_t myID = _inOrderCount++;
            _nodeToId[node] = myID;
            NodeSolution* soln = new NodeSolution();
            _memo[_nodeToId[node]] = soln;

            _curEnum[myID] = 0;

            // Takes ownership.
            soln->andSolution.reset(andSolution);
            return andSolution->subnodes.size() > 0;
        }
        else if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            // TODO: This is done for everything, maybe have NodeSolution* newMemo(node)?
            size_t myID = _inOrderCount++;
            _nodeToId[node] = myID;
            NodeSolution* soln = new NodeSolution();
            _memo[_nodeToId[node]] = soln;

            _curEnum[myID] = 0;

            // Fill out the NodeSolution.
            soln->pred.reset(new PredicateSolution());
            if (NULL != node->getTag()) {
                RelevantTag* rt = static_cast<RelevantTag*>(node->getTag());
                soln->pred->first.swap(rt->first);
                soln->pred->notFirst.swap(rt->notFirst);
            }
            soln->pred->expr = node;
            // There's no guarantee that we can use any of the notFirst indices, so we only claim to
            // be indexed when there are 'first' indices.
            return soln->pred->first.size() > 0;
        }
        else if (node->isLogical()) {
            if (MatchExpression::OR == node->matchType()) {
                // For an OR to be indexed all its children must be indexed.
                bool indexed = true;
                for (size_t i = 0; i < node->numChildren(); ++i) {
                    if (!prepMemo(node->getChild(i))) {
                        indexed = false;
                    }
                }

                size_t myID = _inOrderCount++;
                _nodeToId[node] = myID;
                NodeSolution* soln = new NodeSolution();
                _memo[_nodeToId[node]] = soln;

                OrSolution* orSolution = new OrSolution();
                for (size_t i = 0; i < node->numChildren(); ++i) {
                    orSolution->subnodes.push_back(_nodeToId[node->getChild(i)]);
                }
                soln->orSolution.reset(orSolution);
                return indexed;
            }
            else {
                // To be exhaustive, we would compute all solutions of size 1, 2, ...,
                // node->numChildren().  Each of these solutions would get a place in the
                // memo structure.

                // If there is a geoNear, we put it at the start of our options to ensure that, even
                // if we enumerate one plan, we will index it.
                //
                // XXX: This is a crappy substitute for something smarter, namely always including
                // geoNear in every possible selection inside of an AND.  Revisit when we enum
                // more than one plan.
                size_t geoNearChild = IndexTag::kNoIndex;
                
                // For efficiency concerns, we don't explore any more than the size-1 members
                // of the power set.  That is, we will only use one index at a time.
                AndSolution* andSolution = new AndSolution();
                for (size_t i = 0; i < node->numChildren(); ++i) {
                    // If AND requires an index it can only piggyback on the children that have
                    // indices.
                    if (prepMemo(node->getChild(i))) {
                        vector<size_t> option;
                        size_t childID = _nodeToId[node->getChild(i)];
                        option.push_back(childID);
                        andSolution->subnodes.push_back(option);

                        // Fill out geoNearChild, possibly.
                        // TODO: Actually rank enumeration aside from "always pick GEO_NEAR".
                        if (NULL != _memo[childID]) {
                            NodeSolution* ns = _memo[childID];
                            if (NULL != ns->pred) {
                                verify(NULL != ns->pred->expr);
                                if (MatchExpression::GEO_NEAR == ns->pred->expr->matchType()) {
                                    geoNearChild = andSolution->subnodes.size() - 1;
                                }
                            }
                        }
                    }
                }

                if (IndexTag::kNoIndex != geoNearChild && (0 != geoNearChild)) {
                    andSolution->subnodes[0].swap(andSolution->subnodes[geoNearChild]);
                }

                size_t myID = _inOrderCount++;
                _nodeToId[node] = myID;
                NodeSolution* soln = new NodeSolution();
                _memo[_nodeToId[node]] = soln;

                verify(MatchExpression::AND == node->matchType());
                _curEnum[myID] = 0;

                // Takes ownership.
                soln->andSolution.reset(andSolution);
                return andSolution->subnodes.size() > 0;
            }
        }
        return false;
    }

    void PlanEnumerator::tagMemo(size_t id) {
        NodeSolution* soln = _memo[id];
        verify(NULL != soln);

        if (NULL != soln->pred) {
            verify(NULL == soln->pred->expr->getTag());
            // There may be no indices assignable.  That's OK.
            if (0 != soln->pred->first.size()) {
                // We only assign indices that can be used without any other predicate.
                // Compound is dealt with in the AND processing; there must be an AND to use
                // a notFirst index..
                verify(_curEnum[id] < soln->pred->first.size());
                soln->pred->expr->setTag(new IndexTag(soln->pred->first[_curEnum[id]]));
            }
        }
        else if (NULL != soln->orSolution) {
            for (size_t i = 0; i < soln->orSolution->subnodes.size(); ++i) {
                tagMemo(soln->orSolution->subnodes[i]);
            }
            // TODO: Who checks to make sure that we tag all nodes of an OR?  We should
            // know this early.
        }
        else {
            verify(NULL != soln->andSolution);
            verify(_curEnum[id] < soln->andSolution->subnodes.size());
            vector<size_t> &cur = soln->andSolution->subnodes[_curEnum[id]];

            for (size_t i = 0; i < cur.size(); ++i) {
                // Tag the child.
                tagMemo(cur[i]);
            }
        }
    }

    bool PlanEnumerator::nextMemo(size_t id) {
        return false;
    }

} // namespace mongo
