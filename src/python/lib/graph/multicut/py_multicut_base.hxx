#pragma once
#ifndef NIFTY_PYTHON_GRAPH_MULTICUT_MULTICUT_BASE_HXX
#define NIFTY_PYTHON_GRAPH_MULTICUT_MULTICUT_BASE_HXX

#include "nifty/graph/multicut/multicut_base.hxx"

namespace nifty {
namespace graph {








template<class OBJECTIVE>
class PyMulticutBase : public MulticutBase<OBJECTIVE> {
public:
    /* Inherit the constructors */
    // using MulticutFactory<Objective>::MulticutFactory;

    typedef OBJECTIVE Objective;
    typedef MulticutVisitorBase<OBJECTIVE> VisitorBase;
    typedef MulticutBase<Objective> McBase;
    typedef typename Objective::Graph Graph;
    typedef typename Graph:: template EdgeMap<uint8_t>  EdgeLabels;
    typedef typename Graph:: template NodeMap<uint64_t> NodeLabels;


    /* Trampoline (need one for each virtual function) */
    void optimize(NodeLabels & nodeLabels, VisitorBase * visitor) {
        PYBIND11_OVERLOAD_PURE(
            void,                  /* Return type */
            McBase,                /* Parent class */
            optimize,              /* Name of function */
            nodeLabels,  visitor   /* Argument(s) */
        );
    }

    const Objective & objective() const {
        PYBIND11_OVERLOAD_PURE(
            const Objective & ,    /* Return type */
            McBase,                /* Parent class */
            objective              /* Name of function */
        );
    }
};


} // namespace graph
} // namespace nifty

#endif /* NIFTY_PYTHON_GRAPH_MULTICUT_MULTICUT_BASE_HXX */
