#include "seahorn/config.h"
#include "seahorn/PathBasedBmc.hh"
#include "seahorn/UfoSymExec.hh"
#include "seahorn/Support/CFG.hh"
#include "seahorn/Analysis/CutPointGraph.hh"

#include "ufo/Stats.hh"

#ifdef HAVE_CRAB_LLVM
#include "seahorn/LoadCrab.hh"
#include "crab_llvm/CrabLlvm.hh"
#include "crab_llvm/HeapAbstraction.hh"
#include "crab_llvm/wrapper_domain.hh"
#endif

#include "llvm/Support/CommandLine.h"

/** 
  Important: Certain parts of this implementation is VC
  encoding-dependent. For instance, the generation of blocking clauses
  and the boolean abstraction. Currently, it has been testing with
  UfoLargeSymExec and the following options enabled:

    --horn-split-only-critical=true
    --horn-at-most-one-predecessor=true
**/


static llvm::cl::opt<bool>
UseCrab ("horn-bmc-crab",
	 llvm::cl::desc ("Use of Crab in BMC (restricted to the path-based engine)"), 
	 llvm::cl::init (false));


namespace seahorn
{
  enum muc_method_t { MUC_NAIVE, MUC_ASSUMPTIONS, MUC_BINARY_SEARCH };
}


namespace seahorn
{

  // Return true if e is a tuple
  static bool isTuple(Expr e)
  { return expr::op::bind::isFdecl(e->left()) && isOpX<TUPLE>(e->left()->left()); }

  // Retun the tuple elements as a pair
  static std::pair<Expr, Expr> getTuple(Expr e) {
    assert(isTuple(e));
    Expr tuple = e->left()->left();
    Expr src = tuple->left();
    Expr dst = tuple->right();
    return std::make_pair(src,dst);
  }
  
  // Customized ordering to ensure that non-tuple expressions come
  // first than tuple expressions, otherwise standard ordering between
  // Expr's.
  struct lessExpr {
    bool operator()(Expr e1, Expr e2) const {
      if (!isTuple(e1) && isTuple(e2)) return true;
      else if (isTuple(e1) && !isTuple(e2)) return false;
      else return e1 < e2;
    }
  };

  // A CFG edge is critical if it is not the only leaving its source
  // block and the only entering to the destination block.
  static bool isCriticalEdge(const BasicBlock* src, const BasicBlock* dst) {
    bool not_only_leaving = false;
    bool not_only_entering = false;

    for (const BasicBlock* s : seahorn::succs (*src)) {
      if (s != dst) {
	not_only_leaving = true;
	break;
      }
    }
    
    for (const BasicBlock* p : seahorn::preds (*dst)) {
      if (p != src) {
	not_only_entering = true;
	break;
      }
    }
    
    return (not_only_leaving && not_only_entering);
  }

  // Remove all boolean operators except AND/OR/NEG
  struct PreNNF : public std::unary_function<Expr,Expr> {
    PreNNF(){}
    
    Expr operator() (Expr exp) {

      if (!isOp<BoolOp> (exp)) {
	return exp;
      }
      
      if (!isOpX<IMPL>(exp) && !isOpX<ITE>(exp) && !isOpX<IFF>(exp) && !isOpX<XOR>(exp)) {
	return exp;
      }

      if (isOpX<XOR>(exp)) {
	assert(false && "TODO");
	return exp;
      } else if (isOpX<IMPL>(exp)) {
	return op::boolop::lor(op::boolop::lneg(exp->left()), exp->right());
      } else if (isOpX<ITE>(exp)) {
	assert(exp->arity() == 3);
	Expr c  = exp->operator[](0);
	Expr e1 = exp->operator[](1);
	Expr e2 = exp->operator[](2);	
	return op::boolop::lor(op::boolop::land(c, e1),
			       op::boolop::land(op::boolop::lneg(c), e2));
      } else {
	assert (isOpX<IFF>(exp));
	return op::boolop::land(op::boolop::lor(op::boolop::lneg(exp->left()), exp->right()),
				op::boolop::lor(op::boolop::lneg(exp->right()), exp->left()));
      }
    }
  };

  // Perform boolean abstraction
  struct BA: public std::unary_function<Expr,VisitAction> {

    bool is_pos_bool_lit(Expr e) const {
      return (isOpX<TRUE>(e) ||
	      isOpX<FALSE>(e) ||
	      bind::isBoolConst(e));
    }

    bool is_neg_bool_lit(Expr e) const {
      return (isOpX<NEG>(e) && is_pos_bool_lit(e->left()));
    }
    
    bool is_bool_lit(Expr e) const {
      return (is_pos_bool_lit(e) || is_neg_bool_lit(e));
    }

    ExprFactory &efac;
    std::shared_ptr<op::boolop::TrivialSimplifier> r;

    BA(const BA& o)
      : efac(o.efac), r(o.r) {}

    BA(ExprFactory &fac)
      : efac(fac), r(std::make_shared<op::boolop::TrivialSimplifier>(efac)) {}

    // Pre: exp is in NNF
    VisitAction operator() (Expr exp) {
      if (is_pos_bool_lit(exp)) {
	return VisitAction::skipKids();
      }

      if (isOpX<NEG>(exp)) {
	if (is_pos_bool_lit(exp->left())) {
	  return VisitAction::doKids();	  
	} else {
	  return VisitAction::changeTo(r->trueE);
	}
      }
      
      if (isOpX<AND>(exp) || isOpX<OR>(exp)) {
	return VisitAction::doKids();
      }

      if (isOpX<EQ>(exp)) {
	if (is_bool_lit(exp->left()) && is_bool_lit(exp->right())) {
	  return VisitAction::doKids();
	}
      }

      // everything else abstracted to true
      return VisitAction::changeTo(r->trueE);
    }
  };

  static Expr pre_nnf(Expr exp) {
    op::boolop::BS<PreNNF> bs (new PreNNF());
    return dagVisit(bs, exp);
  }

  static Expr bool_abstraction(Expr exp) {
    exp = pre_nnf(exp);
    exp = op::boolop::nnf(exp);    
    BA n(exp->efac());
    return dagVisit(n, exp);
  }

  static void bool_abstraction(ExprVector& side, ExprVector& abs_side) {
    for (auto exp: side) {
      Expr bexp = bool_abstraction(exp);
      abs_side.push_back(bexp);
    }
    abs_side.erase(std::remove_if(abs_side.begin(), abs_side.end(),
				  [](Expr e){return isOpX<TRUE>(e);}),abs_side.end());    
  }

  // Compute minimal unsatisfiable cores
  class minimal_unsat_core {
  protected:    
    ufo::ZSolver<ufo::EZ3>& m_solver;
    unsigned m_num_solver_calls;
    
  public:
    minimal_unsat_core(ufo::ZSolver<ufo::EZ3>& solver)
      : m_solver(solver), m_num_solver_calls(0) {}
    
    virtual void run(const ExprVector& f, ExprVector& core) = 0;

    virtual std::string get_name(void) const = 0;
    
    void print_stats(llvm::raw_ostream& o) {
      o << get_name() << "\n";
      o << "\t" << m_num_solver_calls << " number of solver calls\n";
    }
  };


  class muc_with_assumptions: public minimal_unsat_core {
  public:
    muc_with_assumptions(ufo::ZSolver<ufo::EZ3>& solver)
      : minimal_unsat_core(solver) {}

    std::string get_name(void) const {
      return "MUC with assumptions";
    }
    
    void run(const ExprVector& f, ExprVector& core) override {
      bmc_impl::unsat_core(m_solver, f, core);
    }
  };
  
  class binary_search_muc;

  // Naive quadratic method
  class naive_muc: public minimal_unsat_core {
    friend class binary_search_muc;
  private:
    typedef ExprVector::const_iterator const_iterator;
    boost::tribool check(const_iterator it, const_iterator et,
			 const ExprVector& assumptions) {
      m_solver.reset();
      for (Expr e: assumptions) {
	m_solver.assertExpr(e);
      }
      for (Expr e: boost::make_iterator_range (it, et)) {
	m_solver.assertExpr(e);
      }
      m_num_solver_calls++;
      return m_solver.solve();
    }

    // TODO: incremental version    
    void run(const ExprVector& f, const ExprVector& assumptions, ExprVector& out) {
      assert (!((bool) check(f.begin(), f.end(), assumptions))); 

      out.insert(out.end(), f.begin(), f.end());
      for (unsigned i = 0; i < out.size ();) {
	Expr saved = out [i];
	out [i] = out.back ();
	boost::tribool res = check(out.begin (), out.end () - 1, assumptions);
	if (res) {
	  out [i++] = saved;
	} else if (!res) {
	  out.pop_back ();
	} else {
	  assert(false);
	}
      }      
    }
    
  public:
    naive_muc(ufo::ZSolver<ufo::EZ3>& solver)
      : minimal_unsat_core(solver) {}

    
    void run(const ExprVector& f, ExprVector& out) override {
      ExprVector assumptions;
      run(f, assumptions, out);
    }

    std::string get_name() const override {
      return "Naive MUC";
    }
    
  };

  // Compute minimal unsatisfiable cores using binary search
  class binary_search_muc: public minimal_unsat_core{
  private:

    // minimum size of the formula to perform binary search on it
    const unsigned threshold = 10;
    
    typedef typename ExprVector::const_iterator const_iterator;
    
    boost::tribool check_with_assumptions(const_iterator it, const_iterator et,
					  const ExprVector& assumptions) {
      m_solver.reset();
      for (Expr e: assumptions) {
	m_solver.assertExpr(e);
      }
      for (const_iterator i=it; i!=et; ++i) {
	m_solver.assertExpr(*i);
      }
      m_num_solver_calls++;
      return m_solver.solve();
    }

    void run_with_assumptions(const_iterator f_beg, const_iterator f_end,
			      const ExprVector& assumptions, ExprVector& core) { 
      assert(!(bool) check_with_assumptions(f_beg, f_end, assumptions));

      unsigned size = std::distance(f_beg, f_end);
      if (size <= threshold) {
	if (size == 0) {
	} else if (size == 1) {
	  core.reserve(std::distance(f_beg, f_end));
	  core.insert(core.end(), f_beg, f_end);
	} else {
	  naive_muc muc(m_solver);
	  ExprVector small_f(f_beg, f_end);
	  ExprVector small_core;
	  muc.run(small_f, assumptions, small_core);
	  core.insert(core.end(), small_core.begin(), small_core.end());
	  m_num_solver_calls += muc.m_num_solver_calls;
	}
	return;
      }

      const_iterator A_beg = f_beg;
      const_iterator A_end = f_beg +  (size/2);      
      const_iterator B_beg = A_end;
      const_iterator B_end = f_end;
      
      boost::tribool resA = boost::indeterminate;      
      boost::tribool resB = boost::indeterminate;
      
      /*A*/
      resA = check_with_assumptions(A_beg, A_end,  assumptions);
      if (!resA) {
	return run_with_assumptions(A_beg, A_end, assumptions, core);
      } else if (resA) {
	/* B */
	resB = check_with_assumptions(B_beg, B_end, assumptions);
	if (!resB) {
	  return run_with_assumptions(B_beg, B_end, assumptions, core);
	} else if (resB) {
	  /* do nothing */
	} else {
	  assert(false);
	}
      } else {
	assert(false);
      }
      assert((bool) resA && (bool) resB);

      unsigned num_assumes = assumptions.size();
      ExprVector new_assumptions(assumptions.begin(), assumptions.end());
      
      // minimize A assuming B (plus assumptions) is an unsat core
      new_assumptions.insert(new_assumptions.end(), B_beg, B_end);
      run_with_assumptions(A_beg, A_end, new_assumptions, core);
      
      // minimize B assuming core (plus assumptions) is an unsat core
      new_assumptions.erase(new_assumptions.begin() + num_assumes, new_assumptions.end());
      new_assumptions.insert(new_assumptions.end(), core.begin(), core.end());
      run_with_assumptions(B_beg, B_end, new_assumptions, core);
    }

  public:
    binary_search_muc(ufo::ZSolver<ufo::EZ3>& solver): minimal_unsat_core(solver) {}
    
    void run(const ExprVector& f, ExprVector& out) override {
      ExprVector assumptions;
      run_with_assumptions(f.begin(), f.end(), assumptions, out);
    }

    std::string get_name() const override {
      return "Binary search-based MUC";
    }
    
  };
  
  #ifdef HAVE_CRAB_LLVM 
  /* 
     It builds a sliced Crab CFG wrt trace and performs abstract
     interpretation on it. This sliced CFG should correspond to a path
     in the original CFG.

     Return false iff the abstract interpretation of path is
     bottom. If bottom then it computes a blocking clause for that
     path.

     Modify m_active_bool_lits.

     NOTE: Currently, blocking clause is Boolean since the only
     abstraction we handle is Boolean.
   */   
  bool PathBasedBmcEngine::path_encoding_and_solve_with_ai(BmcTrace& trace,
							   invariants_map_t& path_constraints) {
    using namespace crab_llvm;
    const Function& fun = *(this->m_fn);
    std::vector<const llvm::BasicBlock *> trace_blocks;
    
    LOG("bmc-ai", errs () << "Trace=";);
    for (int i = 0; i < trace.size(); i++) {
      trace_blocks.push_back(trace.bb(i));
      LOG("bmc-ai", errs () << trace.bb(i)->getName() << "  ";);      
    }
    LOG("bmc-ai", errs () << "\n";);	


    // -- crab parameters
    AnalysisParams params;
    // TODO: make these options user options
    //params.dom=TERMS_INTERVALS; // EQ+UF+INTERVALS
    //params.dom=WRAPPED_INTERVALS;
    params.dom=INTERVALS;    
    
    // -- run crab on the path:
    //    If bottom is inferred then relevant_stmts is a minimal subset of
    //    statements along the path that still implies bottom.
    typename IntraCrabLlvm::invariant_map_t postmap, premap;
    std::vector<crab::cfg::statement_wrapper> relevant_stmts;
    // XXX: disabled temporary
    const bool populate_constraints_map = false;
    bool res;
    if (populate_constraints_map) {
      res = m_crab_path->path_analyze(params, trace_blocks, relevant_stmts, postmap, premap);
    } else {
      // we don't compute forward/backward constraints along the path
      res = m_crab_path->path_analyze(params, trace_blocks, relevant_stmts);
    }

    if (populate_constraints_map) {
      // -- Convert crab linear constraints to Expr
      for (const BasicBlock* b : trace_blocks) {
	const ExprVector &live = m_ls->live(b);
	LinConsToExpr conv(*(m_crab_global->get_heap_abstraction()), fun, live);
	ExprVector f;	
	auto it = postmap.find(b);
	if (it != postmap.end()) {
	  lin_cst_sys_t csts = it->second->to_linear_constraints();
	  for(auto cst: csts) {
	    Expr e = conv.toExpr(cst, sem().efac());
	    if (isOpX<FALSE>(e)) {
	      f.clear();
	      f.push_back(e);
	    } else if (isOpX<TRUE>(e)) {
	      continue;
	    } else {
	      f.push_back(e);
	    }
	  }
	} else {
	  // if the map key is not found then the value is assumed to be
	  // bottom.
	  f.push_back(mk<FALSE>(sem().efac()));
	}
	path_constraints.insert(std::make_pair(b, f));
      }
    }

    if (!res) {

      LOG("bmc", errs () << "Crab proved unsat!\n";);      
      ufo::Stats::resume ("BMC path-based: boolean blocking clause");
      
      LOG("bmc-ai",
	  errs () << "\nRelevant Crab statements:\n";
	  for(auto &s: relevant_stmts) {
	    errs () << s.m_parent.get_name();
	    if (s.m_parent.is_edge()) {
	      auto e= s.m_parent.get_edge();
	      errs () << " (" << e.first->getName() << "," << e.second->getName() << ")";
	    }
	    errs () << ":\n";
	    crab::outs () << "\t" << *(s.m_s) << "\n";
	  });

      // TODO: right now we don't use necessary
      // preconditions. However, if we would use an abstraction that
      // can express intervals and/or equalities, we could use these
      // preconditions as blocking clauses.
      
      LOG("bmc-ai",
	  if (populate_constraints_map) {
	    errs() << "\nNecessary preconditions:\n";
	    for(auto &kv: premap) {
	      crab::outs() << kv.first->getName() << ": " << kv.second << "\n";
	    }
	  });
      
      // -- Refine the Boolean abstraction from a minimal infeasible
      //    sequence of crab statements.
      
      /* 
	 1) A binary operation s at bb is translated to (bb => s)
         2) Most assignments are coming from PHI nodes:
            At bi, given "x = PHI (y, bj) (...)" crab translates it into x = y at bj.
            So we can translate it into ((bj and (bj and bi)) => x=y)
         3) assume(cst) at bbi is coming from
	    "f = ICMP cst at bb, BR f bbi, bbj", then we produce:
            ((bb and (bb and bbi)) => f)

	 We need to take special care if an edge is critical:
   	  - a PHI node is translated into bj and tuple(bi,bj) => x=y
          - a branch is translated into b and tuple(bb,bbi) => f
      */
      
      std::set<Expr, lessExpr> active_bool_lits;
      for (auto it = relevant_stmts.begin(); it != relevant_stmts.end(); ++it) {
       	crab::cfg::statement_wrapper s = *it;
	if (s.m_s->is_bin_op() || s.m_s->is_int_cast() || s.m_s->is_select() ||
	    s.m_s->is_bool_bin_op() || s.m_s->is_bool_assign_cst() ||
	    s.m_s->is_arr_write() || s.m_s->is_arr_read() ||
	    // array assumptions are not coming from branches
	    s.m_s->is_arr_assume() ||
	    // array assignments are not coming from PHI nodes
	    s.m_s->is_arr_assign()) {
	  const BasicBlock* BB = s.m_parent.get_basic_block(); 
	  assert(BB);
	  active_bool_lits.insert(sem().symb(*BB));
	  continue;
	} else if (s.m_s->is_assume() || s.m_s->is_bool_assume()) {
	  if (s.m_parent.is_edge()) {
	    auto p = s.m_parent.get_edge();
	    Expr src = sem().symb(*p.first);
	    Expr dst = sem().symb(*p.second);

	    Expr edge;
	    if (isCriticalEdge(p.first, p.second)) {
	      edge = bind::boolConst(mk<TUPLE>(src,dst));
	    } else {
	      edge = mk<AND>(src, dst);
	    }
	    active_bool_lits.insert(src);	    
	    active_bool_lits.insert(edge);
	  } else {
	    assert(s.m_s->is_bool_assume());
	    const BasicBlock* BB = s.m_parent.get_basic_block(); 
	    active_bool_lits.insert(sem().symb(*BB));
	  }
	  continue;	  
	} else if (s.m_s->is_assign() || s.m_s->is_bool_assign_var()) {
	  const PHINode * Phi = nullptr;
	  
	  if (s.m_s->is_assign()) {
	    typedef typename cfg_ref_t::basic_block_t::assign_t assign_t;	  
	    auto assign = static_cast<const assign_t*>(s.m_s);
	    if (boost::optional<const llvm::Value*> lhs = assign->lhs().name().get()) {
	      Phi = dyn_cast<PHINode>(*lhs);
	    }
	  } else {
	    typedef typename cfg_ref_t::basic_block_t::bool_assign_var_t bool_assign_var_t;
	    auto assign = static_cast<const bool_assign_var_t*>(s.m_s);
	    if (boost::optional<const llvm::Value*> lhs = assign->lhs().name().get()) {
	      Phi = dyn_cast<PHINode>(*lhs);
	    }
	  }
	  if (Phi) {
	    
	    const BasicBlock* src_BB = s.m_parent.get_basic_block();
	    if (!src_BB) {
	      src_BB = s.m_parent.get_edge().first;
	    }
	    
	    assert(src_BB);
	    const BasicBlock* dst_BB = Phi->getParent();
	    Expr src = sem().symb(*src_BB);
	    Expr dst = sem().symb(*dst_BB);

	    Expr edge;
	    if (isCriticalEdge(src_BB, dst_BB)) {
	      edge = bind::boolConst(mk<TUPLE>(src,dst));
	    } else {
	      edge = mk<AND>(src, dst);
	    }
	    active_bool_lits.insert(src);	    
	    active_bool_lits.insert(edge);	    
	    continue;
	  }
	}

	// sanity check: this should not happen.
	crab::outs() << "TODO: inference of active bool literals for " << *s.m_s << "\n";
	// By returning true we pretend the query was sat so we run
	// the SMT solver next.
	ufo::Stats::stop ("BMC path-based: boolean blocking clause");	
	return true;	  
      }
      
      // -- Finally, evaluate the symbolic boolean variables in their
      //    corresponding symbolic stores.

      // Symbolic states are associated with cutpoints
      m_active_bool_lits.clear();      
      auto &cps = getCps();
      std::vector<SymStore>& states = getStates();
      for (Expr e : active_bool_lits) {
	// Find the state where e is defined.
	// XXX: this is expensive but don't know how to do it better.
	bool found = false;
	for (unsigned i=0; i < cps.size(); ++i) {
	  const CutPoint *cp = cps[i];
	  SymStore &s = states[i];
	  Expr v = s.eval(e);
	  if (v != e) {
	    m_active_bool_lits.push_back(v);
	    found = true;
	    break;
	  }
	  if (isTuple(e)) {
	    // s.eval does not traverse function declarations
	    auto t = getTuple(e);
	    if (s.isDefined(t.first) && s.isDefined(t.second)) {
	      m_active_bool_lits.push_back(bind::boolConst(mk<TUPLE>(s.eval(t.first),
							       s.eval(t.second))));
	      found = true;
	      break;
	    }
	  }
	}
	if (!found) {
	  // Sanity check
	  errs() << "Path-based BMC ERROR: cannot produce an unsat core from Crab\n";
	  // XXX: we continue and pretend the query was satisfiable so
	  // nothing really happens and the smt solver is used next.
	  return true;
	}
      }
      ufo::Stats::stop ("BMC path-based: boolean blocking clause");	      
    }    
    return res;
  }
  #endif
    
  /*
    First, it builds an implicant of the precise encoding (m_side)
    with respect to the model. This implicant should correspond to a
    path. Then, it checks that the implicant is unsatisfiable. If yes,
    it produces a blocking clause for that path. Otherwise, it
    produces a model.

    Modify: m_aux_smt_solver, m_active_bool_lits, and m_model.

    NOTE: Currently, blocking clauses are Boolean since the only
    abstraction we handle is Boolean.
  */      
  boost::tribool PathBasedBmcEngine::
  path_encoding_and_solve_with_smt(ufo::ZModel<ufo::EZ3> &model,
				   const invariants_map_t& /*invariants*/,
				   // extra constraints inferred by
				   // crab for current implicant
				   const invariants_map_t& /*path_constraints*/) {
    
    ExprVector path_formula, assumptions;
    path_formula.reserve(m_side.size());

    // make a copy of m_side since get_model_implicant modifies it.
    ExprVector f(m_side.begin(), m_side.end());

    ExprMap active_bool_map;

    // TODO: BmcTrace already computed the implicant so we might
    // compute the implicant twice if crab is enabled.
    bmc_impl::get_model_implicant(f, model, path_formula, active_bool_map);
    // remove redundant literals
    std::sort(path_formula.begin(), path_formula.end());
    path_formula.erase(std::unique(path_formula.begin(), path_formula.end()),
		       path_formula.end());

    LOG("bmc", errs() << "PATH FORMULA:\n";
	for (Expr e: path_formula) { errs () << "\t" << *e << "\n"; });
	  
    /*****************************************************************
     * This check might be expensive if path_formula contains complex
     * bitvector/floating point expressions.
     * TODO: make decisions `a la` mcsat to solve faster. We will use
     * here invariants to make only those decisions which are
     * consistent with the invariants.
     *****************************************************************/
    m_aux_smt_solver.reset();
    for (Expr e: path_formula) {
      m_aux_smt_solver.assertExpr(e);
    }
    // TODO: add here path_constraints to help
    boost::tribool res = m_aux_smt_solver.solve();

    if (!res) {
      ufo::Stats::resume ("BMC path-based: SMT unsat core");
      
      // --- Compute minimal unsat core of the path formula
      muc_method_t muc_method = MUC_ASSUMPTIONS;
      ExprVector unsat_core;      
      switch(muc_method) {
      case MUC_ASSUMPTIONS: {
      	muc_with_assumptions muc(m_aux_smt_solver);
      	muc.run(path_formula, unsat_core);
	LOG("bmc-unsat-core", muc.print_stats(errs()));
	break;
      }
      case MUC_NAIVE: {
      	naive_muc muc(m_aux_smt_solver);
      	muc.run(path_formula, unsat_core);
	LOG("bmc-unsat-core", muc.print_stats(errs()));
	break;
      }
      case MUC_BINARY_SEARCH: {
      	binary_search_muc muc(m_aux_smt_solver);
      	muc.run(path_formula, unsat_core);
	LOG("bmc-unsat-core", muc.print_stats(errs()));	
	break;
      }
      default: assert(false);
      }
      ufo::Stats::stop ("BMC path-based: SMT unsat core");

      ufo::Stats::resume ("BMC path-based: boolean blocking clause");            
      // -- Refine the Boolean abstraction using the unsat core
      ExprSet active_bool_set;
      for(Expr e: unsat_core) {
	auto it = active_bool_map.find(e);
	// It's possible that an implicant has no active booleans.
	// For instance, corner cases where the whole program is a
	// single block.
	if (it != active_bool_map.end()) {
	  active_bool_set.insert(it->second);
	}
      }
      m_active_bool_lits.assign(active_bool_set.begin(), active_bool_set.end());
      ufo::Stats::stop ("BMC path-based: boolean blocking clause");      

    } else if (res) {
      m_model = m_aux_smt_solver.getModel();
    }    
    return res;
  }

  #ifdef HAVE_CRAB_LLVM
  PathBasedBmcEngine::PathBasedBmcEngine(SmallStepSymExec &sem, ufo::EZ3 &zctx,
					 crab_llvm::CrabLlvmPass *crab,
					 const TargetLibraryInfo& tli)
    : BmcEngine(sem, zctx),
      m_aux_smt_solver(zctx), m_tli(tli), m_model(zctx), m_ls(nullptr),
      m_crab_global(crab), m_crab_path(nullptr) { }
  #else
  PathBasedBmcEngine::PathBasedBmcEngine(SmallStepSymExec &sem, ufo::EZ3 &zctx,
					 const TargetLibraryInfo& tli)
    : BmcEngine(sem, zctx),
      m_aux_smt_solver(zctx), m_tli(tli), m_model(zctx), m_ls(nullptr) { }
  #endif 

  PathBasedBmcEngine::~PathBasedBmcEngine() {
    if (m_ls) delete m_ls;
    #ifdef HAVE_CRAB_LLVM
    if (m_crab_path) delete m_crab_path;
    #endif 
  }

  void PathBasedBmcEngine::encode() {
    // TODO: for a path-based bmc engine is not clear what to do here.
  }
  
  boost::tribool PathBasedBmcEngine::solve() {
    LOG("bmc", errs () << "Starting path-based BMC \n";);
    
    invariants_map_t invariants;
    
    #ifdef HAVE_CRAB_LLVM
    // Convert crab invariants to Expr

    // -- Compute live symbols so that invariant variables exclude
    //    dead variables.
    m_ls = new LiveSymbols(*m_fn, sem().efac(), sem());
    m_ls->run();
    // -- Translate invariants
    for(const BasicBlock& b: *m_fn) {
      const ExprVector& live = m_ls->live(&b);
      LinConsToExpr conv(*(m_crab_global->get_heap_abstraction()), *m_fn, live);
      crab_llvm::lin_cst_sys_t csts = m_crab_global->get_pre(&b)->to_linear_constraints();
      ExprVector inv;	
      for(auto cst: csts) {
    	Expr e = conv.toExpr(cst, sem().efac());
    	if (isOpX<FALSE>(e)) {
    	  inv.clear();
    	  inv.push_back(e);
    	  break;
    	} else if (isOpX<TRUE>(e)) {
    	  continue;
    	} else {
    	  inv.push_back(e);
    	}
      }
      invariants.insert(std::make_pair(&b, inv));
    }
    
    LOG("bmc-ai",
	for(auto &kv: invariants) {
	  errs () << "Invariants at " << kv.first->getName() << ": ";
	  if (kv.second.empty()) {
	    errs () << "true\n";
	  } else {
	    errs () << "\n";
	    for(auto e: kv.second) {
	      errs () << "\t" << *e << "\n";
	    }
	  }
	});
    
    // Create another instance of crab to analyze single paths
    // TODO: make these options user options
    const crab::cfg::tracked_precision prec_level = crab::cfg::ARR;
    auto heap_abstraction = m_crab_global->get_heap_abstraction();
    // TODO: modify IntraCrabLlvm api so that it takes the cfg already
    // generated by m_crab_global    
    m_crab_path = new crab_llvm::IntraCrabLlvm(*(const_cast<Function*>(this->m_fn)), m_tli,
					       prec_level, heap_abstraction);
    #endif 

    // -- Precise encoding
    ufo::Stats::resume ("BMC path-based: precise encoding");        
    BmcEngine::encode();
    // remove the precise encoding from the solver
    m_smt_solver.reset();
    ufo::Stats::stop ("BMC path-based: precise encoding");            

    LOG("bmc",
	errs()<< "Begin precise encoding:\n";
	for (Expr v : m_side)
	  errs () << "\t" << *v << "\n";
	errs() << "End precise encoding\n";);
	
    // -- Boolean abstraction
    LOG("bmc", errs()<< "Begin boolean abstraction:\n";);
    ufo::Stats::resume ("BMC path-based: initial boolean abstraction");    
    ExprVector abs_side;
    bool_abstraction(m_side, abs_side);
    // XXX: we use m_smt_solver for keeping the abstraction. We do
    //      that so that BmcTrace access to the right solver.
    for (Expr v : abs_side) {
      LOG("bmc", errs () << "\t" << *v << "\n";);
      m_smt_solver.assertExpr (v);
    }
    ufo::Stats::stop ("BMC path-based: initial boolean abstraction");                    
    LOG("bmc", errs()<< "End boolean abstraction:\n";);

    LOG("bmc-progress", errs() << "Processing symbolic path ");
    /**
     * Main loop
     *
     * Use boolean abstraction to enumerate paths. Each time a path is
     * unsat, blocking clauses are added to avoid exploring the same
     * path.
    **/
    unsigned iters = 0;
    while ((bool) (m_result = m_smt_solver.solve())) {
      ++iters;
      ufo::Stats::count("BMC total number of symbolic paths");

      LOG("bmc-progress", errs() << iters << " ");          
      ufo::ZModel<ufo::EZ3> model = m_smt_solver.getModel ();
      
      LOG("bmc", errs () << "Model " << iters << " found: \n" << model << "\n";);

      invariants_map_t path_constraints;
      #ifdef HAVE_CRAB_LLVM
      if (UseCrab) {
	BmcTrace trace(*this, model);
	ufo::Stats::resume ("BMC path-based: solving path with AI (included muc)");	
	bool res = path_encoding_and_solve_with_ai(trace, path_constraints);
	ufo::Stats::stop ("BMC path-based: solving path with AI (included muc)"); 
	if (!res) {
	  bool ok = add_blocking_clauses();
	  if (ok) {
	    ufo::Stats::count("BMC number symbolic paths discharged by AI");
	    continue;
	  } else {
	    errs () << "Path-based BMC ERROR: same blocking clause again " << __LINE__ << "\n";
	    return boost::indeterminate;
	  }
	}

      }
      #endif
      ufo::Stats::resume ("BMC path-based: solving path with SMT (included muc)"); 	      
      boost::tribool res= path_encoding_and_solve_with_smt(model, invariants, path_constraints);
      ufo::Stats::stop ("BMC path-based: solving path with SMT (included muc)"); 	      
      if (res == boost::indeterminate || res) {
        #ifdef HAVE_CRAB_LLVM
	// Temporary: for profiling crab
	crab::CrabStats::PrintBrunch (crab::outs());
        #endif 
	return res;
      } else  {
	bool ok = add_blocking_clauses();
	if (!ok) {
	  errs () << "Path-based BMC ERROR: same blocking clause again " << __LINE__ << "\n";  
	  return boost::indeterminate;
	}
	ufo::Stats::count("BMC number symbolic paths discharged by SMT");	
      }
    }

    #ifdef HAVE_CRAB_LLVM
    // Temporary: for profiling crab
    crab::CrabStats::PrintBrunch (crab::outs());
    #endif 
    
    if (iters == 0) {
      errs () << "\nProgram is trivially unsat: initial boolean abstraction was enough.\n";
    }

    return m_result;
  }

  bool PathBasedBmcEngine::add_blocking_clauses() {
    // For now, we only refine the Boolean abstraction.
    
    Expr bc = mk<FALSE>(sem().efac());
    
    if (m_active_bool_lits.empty()) {
      errs () << "No found active boolean literals. Trivially unsat ... \n";
    } else {
      bc = op::boolop::lneg(op::boolop::land(m_active_bool_lits));
    }
    
    LOG("bmc", errs() << "Added blocking clause: " << *bc << "\n";);
    m_smt_solver.assertExpr(bc);
    auto res = m_blocking_clauses.insert(bc);
    return res.second;
  }
  
  BmcTrace PathBasedBmcEngine::getTrace() {
    return BmcTrace(*this, m_model);
  }

  // This is intending only for debugging purposes
  void PathBasedBmcEngine::unsatCore(ExprVector &out) {
    // TODO: for path-based BMC is not clear what to return.
  }

}
