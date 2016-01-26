/**
 * @file     LPSolver.h
 * @brief    Class used to solve Linear Programming Problems as defined in LP.h
 * @author   Ivan Dario Jimenez
 * @date     1/24/16
 */

#pragma once

#include <gtsam_unstable/linear/LPState.h>
#include <gtsam_unstable/linear/LP.h>
#include <gtsam_unstable/linear/ActiveSetSolver.h>
#include <boost/range/adaptor/map.hpp>
#include <gtsam/linear/VectorValues.h>

namespace gtsam {
typedef std::map<Key, size_t> KeyDimMap;

class LPSolver: public ActiveSetSolver {
  const LP& lp_; //!< the linear programming problem
  KeyDimMap keysDim_; //!< key-dim map of all variables in the constraints, used to create zero priors

public:
  /// Constructor
  LPSolver(const LP& lp) :
      lp_(lp) {
    // Push back factors that are the same in every iteration to the base graph.
    // Those include the equality constraints and zero priors for keys that are not
    // in the cost
    baseGraph_.push_back(lp_.equalities);

    // Collect key-dim map of all variables in the constraints to create their zero priors later
    keysDim_ = collectKeysDim(lp_.equalities);
    KeyDimMap keysDim2 = collectKeysDim(lp_.inequalities);
    keysDim_.insert(keysDim2.begin(), keysDim2.end());

    // Create and push zero priors of constrained variables that do not exist in the cost function
    baseGraph_.push_back(*createZeroPriors(lp_.cost.keys(), keysDim_));

    // Variable index
    equalityVariableIndex_ = VariableIndex(lp_.equalities);
    inequalityVariableIndex_ = VariableIndex(lp_.inequalities);
    constrainedKeys_ = lp_.equalities.keys();
    constrainedKeys_.merge(lp_.inequalities.keys());
  }

  const LP& lp() const {
    return lp_;
  }
  const KeyDimMap& keysDim() const {
    return keysDim_;
  }

  //******************************************************************************
  template<class LinearGraph>
  KeyDimMap collectKeysDim(const LinearGraph& linearGraph) const {
    KeyDimMap keysDim;
    BOOST_FOREACH(const typename LinearGraph::sharedFactor& factor, linearGraph) {
      if (!factor) continue;
      BOOST_FOREACH(Key key, factor->keys())
      keysDim[key] = factor->getDim(factor->find(key));
    }
    return keysDim;
  }

  //******************************************************************************
  /**
   * Create a zero prior for any keys in the graph that don't exist in the cost
   */
  GaussianFactorGraph::shared_ptr createZeroPriors(const KeyVector& costKeys,
      const KeyDimMap& keysDim) const {
    GaussianFactorGraph::shared_ptr graph(new GaussianFactorGraph());
    BOOST_FOREACH(Key key, keysDim | boost::adaptors::map_keys) {
      if (find(costKeys.begin(), costKeys.end(), key) == costKeys.end()) {
        size_t dim = keysDim.at(key);
        graph->push_back(JacobianFactor(key, eye(dim), zero(dim)));
      }
    }
    return graph;
  }

  //******************************************************************************
  LPState iterate(const LPState& state) const {
    // Solve with the current working set
    // LP: project the objective neggradient to the constraint's null space
    // to find the direction to move
    VectorValues newValues = solveWithCurrentWorkingSet(state.values,
        state.workingSet);
    // If we CAN'T move further
    // LP: projection on the constraints' nullspace is zero: we are at a vertex
    if (newValues.equals(state.values, 1e-7)) {
      // Find and remove the bad ineq constraint by computing its lambda
      // Compute lambda from the dual graph
      // LP: project the objective's gradient onto each constraint gradient to obtain the dual scaling factors
      //	is it true??
      GaussianFactorGraph::shared_ptr dualGraph = buildDualGraph(
          state.workingSet, newValues);
      VectorValues duals = dualGraph->optimize();
      // LP: see which ineq constraint has wrong pulling direction, i.e., dual < 0
      int leavingFactor = identifyLeavingConstraint(state.workingSet, duals);
      // If all inequality constraints are satisfied: We have the solution!!
      if (leavingFactor < 0) {
        // TODO If we still have infeasible equality constraints: the problem is over-constrained. No solution!
        // ...
        return LPState(newValues, duals, state.workingSet, true,
            state.iterations + 1);
      } else {
        // Inactivate the leaving constraint
        // LP: remove the bad ineq constraint out of the working set
        InequalityFactorGraph newWorkingSet = state.workingSet;
        newWorkingSet.at(leavingFactor)->inactivate();
        return LPState(newValues, duals, newWorkingSet, false,
            state.iterations + 1);
      }
    } else {
      // If we CAN make some progress, i.e. p_k != 0
      // Adapt stepsize if some inactive constraints complain about this move
      // LP: projection on nullspace is NOT zero:
      // 		find and put a blocking inactive constraint to the working set,
      // 		otherwise the problem is unbounded!!!
      double alpha;
      int factorIx;
      VectorValues p = newValues - state.values;
      boost::tie(alpha, factorIx) = // using 16.41
          computeStepSize(state.workingSet, state.values, p);
      // also add to the working set the one that complains the most
      InequalityFactorGraph newWorkingSet = state.workingSet;
      if (factorIx >= 0)
        newWorkingSet.at(factorIx)->activate();
      // step!
      newValues = state.values + alpha * p;
      return LPState(newValues, state.duals, newWorkingSet, false,
          state.iterations + 1);
    }
  }

  //******************************************************************************
  /**
   * Create the factor ||x-xk - (-g)||^2 where xk is the current feasible solution
   * on the constraint surface and g is the gradient of the linear cost,
   * i.e. -g is the direction we wish to follow to decrease the cost.
   *
   * Essentially, we try to match the direction d = x-xk with -g as much as possible
   * subject to the condition that x needs to be on the constraint surface, i.e., d is
   * along the surface's subspace.
   *
   * The least-square solution of this quadratic subject to a set of linear constraints
   * is the projection of the gradient onto the constraints' subspace
   */
  GaussianFactorGraph::shared_ptr createLeastSquareFactors(
      const LinearCost& cost, const VectorValues& xk) const {
    GaussianFactorGraph::shared_ptr graph(new GaussianFactorGraph());
    KeyVector keys = cost.keys();

    for (LinearCost::const_iterator it = cost.begin(); it != cost.end(); ++it) {
      size_t dim = cost.getDim(it);
      Vector b = xk.at(*it) - cost.getA(it).transpose(); // b = xk-g
      graph->push_back(JacobianFactor(*it, eye(dim), b));
    }

    return graph;
  }

  /// Find solution with the current working set
  VectorValues solveWithCurrentWorkingSet(const VectorValues& xk,
      const InequalityFactorGraph& workingSet) const {
    GaussianFactorGraph workingGraph = baseGraph_; // || X - Xk + g ||^2
    workingGraph.push_back(*createLeastSquareFactors(lp_.cost, xk));

    BOOST_FOREACH(const LinearInequality::shared_ptr& factor, workingSet) {
      if (factor->active()) workingGraph.push_back(factor);
    }
    return workingGraph.optimize();
  }

//******************************************************************************
  JacobianFactor::shared_ptr createDualFactor(Key key,
      const InequalityFactorGraph& workingSet,
      const VectorValues& delta) const {

    // Transpose the A matrix of constrained factors to have the jacobian of the dual key
    TermsContainer Aterms = collectDualJacobians < LinearEquality
        > (key, lp_.equalities, equalityVariableIndex_);
    TermsContainer AtermsInequalities = collectDualJacobians < LinearInequality
        > (key, workingSet, inequalityVariableIndex_);
    Aterms.insert(Aterms.end(), AtermsInequalities.begin(),
        AtermsInequalities.end());

    // Collect the gradients of unconstrained cost factors to the b vector
    if (Aterms.size() > 0) {
      Vector b = zero(delta.at(key).size());
      Factor::const_iterator it = lp_.cost.find(key);
      if (it != lp_.cost.end())
        b = lp_.cost.getA(it).transpose();
      return boost::make_shared < JacobianFactor > (Aterms, b); // compute the least-square approximation of dual variables
    } else {
      return boost::make_shared<JacobianFactor>();
    }
  }

//******************************************************************************
  boost::tuple<double, int> computeStepSize(
      const InequalityFactorGraph& workingSet, const VectorValues& xk,
      const VectorValues& p) const {
    return ActiveSetSolver::computeStepSize(workingSet, xk, p,
        std::numeric_limits<double>::infinity());
  }

//******************************************************************************
  InequalityFactorGraph identifyActiveConstraints(
      const InequalityFactorGraph& inequalities,
      const VectorValues& initialValues, const VectorValues& duals) const {
    InequalityFactorGraph workingSet;
    BOOST_FOREACH(const LinearInequality::shared_ptr& factor, inequalities) {
      LinearInequality::shared_ptr workingFactor(new LinearInequality(*factor));

      double error = workingFactor->error(initialValues);
      // TODO: find a feasible initial point for LPSolver.
      // For now, we just throw an exception
      if (error > 0) throw InfeasibleInitialValues();

      if (fabs(error) < 1e-7) {
        workingFactor->activate();
      }
      else {
        workingFactor->inactivate();
      }
      workingSet.push_back(workingFactor);
    }
    return workingSet;
  }

//******************************************************************************
  /** Optimize with the provided feasible initial values
   * TODO: throw exception if the initial values is not feasible wrt inequality constraints
   */
  pair<VectorValues, VectorValues> optimize(const VectorValues& initialValues,
      const VectorValues& duals = VectorValues()) const {

    // Initialize workingSet from the feasible initialValues
    InequalityFactorGraph workingSet = identifyActiveConstraints(
        lp_.inequalities, initialValues, duals);
    LPState state(initialValues, duals, workingSet, false, 0);

    /// main loop of the solver
    while (!state.converged) {
      state = iterate(state);
    }

    return make_pair(state.values, state.duals);
  }

//******************************************************************************
  /**
   * Optimize without initial values
   * TODO: Find a feasible initial solution wrt inequality constraints
   */
//  pair<VectorValues, VectorValues> optimize() const {
//
//    // Initialize workingSet from the feasible initialValues
//    InequalityFactorGraph workingSet = identifyActiveConstraints(
//        lp_.inequalities, initialValues, duals);
//    LPState state(initialValues, duals, workingSet, false, 0);
//
//    /// main loop of the solver
//    while (!state.converged) {
//      state = iterate(state);
//    }
//
//    return make_pair(state.values, state.duals);
//  }
};
}
