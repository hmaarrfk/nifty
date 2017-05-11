#pragma once

#include <queue>

#include "boost/format.hpp"

#include "nifty/tools/runtime_check.hxx"
#include "nifty/graph/components.hxx"

#include "nifty/graph/optimization/multicut/multicut_base.hxx"
#include "nifty/graph/optimization/multicut/multicut_factory.hxx"
#include "nifty/graph/optimization/multicut/multicut_objective.hxx"
#include "nifty/graph/undirected_list_graph.hxx"
#include "nifty/ufd/ufd.hxx"


#include "nifty/graph/optimization/mincut/mincut_visitor_base.hxx"
#include "nifty/graph/optimization/mincut/mincut_base.hxx"
#include "nifty/graph/optimization/mincut/mincut_factory.hxx"
#include "nifty/graph/optimization/mincut/mincut_objective.hxx"
#include "nifty/graph/undirected_list_graph.hxx"


#include "nifty/max_cut_backend/max_cut_qpbo.hxx"


namespace nifty{
namespace graph{

    /// \cond HIDDEN_SYMBOLS
    namespace detail_cgc{

        template<class OBJECTIVE>
        class SubmodelOptimizer{
        public:
            typedef OBJECTIVE ObjectiveType;
            typedef typename ObjectiveType::WeightType WeightType;
            typedef MulticutBase<ObjectiveType> MulticutBaseType;
            typedef typename ObjectiveType::Graph GraphType;
            typedef typename ObjectiveType::WeightsMap WeightsMapType;
            typedef typename GraphType:: template NodeMap<uint64_t> GlobalNodeToLocal;
            typedef std::vector<uint64_t>                       LocalNodeToGlobal;

            typedef typename GraphType:: template EdgeMap<uint8_t> IsDirtyEdge;


            typedef UndirectedGraph<>                   SubGraph;
            typedef MincutObjective<SubGraph, double>   SubObjective;
            typedef MincutFactoryBase<SubObjective>     SubMcFactoryBase;
            typedef MincutBase<SubObjective>            SubMcBase;
            typedef MincutVerboseVisitor<SubObjective>  SubMcVerboseVisitor;
            typedef MincutEmptyVisitor<SubObjective>    SubEmptyVisitor;
            typedef typename  SubMcBase::NodeLabels     SubNodeLabels;

            struct Optimzie1ReturnType{
                Optimzie1ReturnType(const bool imp, const double val)
                :   improvment(imp),
                    minCutValue(val){
                }
                bool        improvment;
                double      minCutValue;
            };

            struct Optimzie2ReturnType{
                Optimzie2ReturnType(const bool imp, const double val)
                :   improvment(imp),
                    improvedBy(val){
                }
                bool        improvment;
                double      improvedBy;
            };

            SubmodelOptimizer(
                const ObjectiveType  & objective, 
                IsDirtyEdge & isDirtyEdge,
                std::shared_ptr<SubMcFactoryBase> & mincutFactory
            )
            :   objective_(objective),
                graph_(objective.graph()),
                weights_(objective.weights()),
                globalNodeToLocal_(objective.graph()),
                localNodeToGlobal_(objective.graph().numberOfNodes()),
                nLocalNodes_(0),
                nLocalEdges_(0),
                maxCut_(graph_.numberOfNodes(), graph_.numberOfNodes()*2),
                ufd_(),
                isDirtyEdge_(isDirtyEdge),
                mincutFactory_(mincutFactory),
                insideEdges_(),
                borderEdges_()
            {
                isDirtyEdge_.reserve(graph_.numberOfNodes());
                borderEdges_.reserve(graph_.numberOfNodes()/4);
                if(!bool(mincutFactory)){
                    throw std::runtime_error("Cgc mincutFactory shall not be empty");
                }
            }

            template<class NODE_LABELS, class ANCHOR_QUEUE>
            Optimzie1ReturnType optimize1(
                NODE_LABELS & nodeLabels,
                const uint64_t anchorNode,
                ANCHOR_QUEUE & anchorQueue
            ){
                // get mapping from local to global and vice versa
                // also counts nLocalEdges
                const auto maxNodeLabel = this->varMapping(nodeLabels, anchorNode);

                maxCut_.assign(nLocalNodes_, nLocalEdges_);
                ufd_.assign(nLocalNodes_);

                if(nLocalNodes_ >= 2){


                    const auto anchorLabel = nodeLabels[anchorNode];

                    // setup the submodel
                    SubGraph        subGraph(nLocalNodes_);
                    SubObjective    subObjective(subGraph);
                    auto &          subWeights = subObjective.weights();

                    this->forEachInternalEdge(nodeLabels, anchorLabel,[&](const uint64_t uLocal, const uint64_t vLocal, const uint64_t edge){
                        const auto w = weights_[edge];
                        subGraph.insertEdge(uLocal, vLocal);
                        subWeights.push_back(w);
                    });
                    // solve it
                    SubNodeLabels subgraphRes(subGraph);
                    auto solverPtr = mincutFactory_->createRawPtr(subObjective);

                    //SubMcVerboseVisitor visitor;
                    solverPtr->optimize(subgraphRes,nullptr);
                    const auto minCutValue  = subObjective.evalNodeLabels(subgraphRes);
                    delete solverPtr;   


                    Optimzie1ReturnType res(false,minCutValue);

                    if(minCutValue < 0.0){
                        //std::cout<<"minCutValue "<<minCutValue<<"\n";

                        this->forEachInternalEdge(nodeLabels, anchorLabel,[&](const uint64_t uLocal, const uint64_t vLocal, const uint64_t edge){
                            if(maxCut_.label(uLocal) == subgraphRes[vLocal]){
                                ufd_.merge(uLocal, vLocal);
                            }
                        });
                   
                        res.improvment = true;
                        res.minCutValue = minCutValue;
                        std::unordered_map<uint64_t,uint64_t> mapping;
                        ufd_.representativeLabeling(mapping);

                        std::vector<uint64_t> anchors(mapping.size());
                        std::vector<uint64_t> anchorsSize(mapping.size(),0);

                        for(auto localNode=0; localNode<nLocalNodes_; ++localNode){
                            const auto node = localNodeToGlobal_[localNode];
                            const auto denseLocalLabel =  mapping[ufd_.find(localNode)];
                            anchorsSize[denseLocalLabel] +=1;
                            anchors[denseLocalLabel] = node;
                            nodeLabels[node] = denseLocalLabel + maxNodeLabel + 1;
                        }

                        for(auto i=0; i<anchors.size(); ++i){
                            if(anchorsSize[i] > 1){
                                res.improvment = true;
                                anchorQueue.push(anchors[i]);
                            }
                        }
                        
                        //std::cout<<"ret.minCutValue "<<res.minCutValue<<"\n";
                        //std::cout<<"ret.improvment "<<res.improvment<<"\n";
                    }   
                    return res;
                }
                else{   
                    return Optimzie1ReturnType(false,0.0);
                }
            }       

            template<class NODE_LABELS>
            Optimzie2ReturnType optimize2(
                NODE_LABELS & nodeLabels,
                const uint64_t anchorNode0, 
                const uint64_t anchorNode1
            ){
                // get mapping from local to global and vice versa
                // also counts nLocalEdges
                const auto maxNodeLabel = this->varMapping(nodeLabels, anchorNode0, anchorNode1);

                // setup the submodel
                SubGraph        subGraph(nLocalNodes_);
                SubObjective    subObjective(subGraph);
                auto &          subWeights = subObjective.weights();

                ufd_.assign(nLocalNodes_);


                const auto anchorLabel0 = nodeLabels[anchorNode0];
                const auto anchorLabel1 = nodeLabels[anchorNode1];

                auto currentCutValue = 0.0;

                this->forEachInternalEdge(nodeLabels, anchorLabel0, anchorLabel1,[&](const uint64_t uLocal, const uint64_t vLocal, const uint64_t edge){
                    const auto w = weights_[edge];
                    subGraph.insertEdge(uLocal, vLocal);
                    subWeights.push_back(w);
                    if(nodeLabels[localNodeToGlobal_[uLocal]] != nodeLabels[localNodeToGlobal_[vLocal]]){
                        currentCutValue += w;
                    }
                });

                // optimize
                SubNodeLabels subgraphRes(subGraph);
                auto solverPtr = mincutFactory_->createRawPtr(subObjective);
                solverPtr->optimize(subgraphRes,nullptr);
                const auto minCutValue  = subObjective.evalNodeLabels(subgraphRes);
                delete solverPtr;   

                

                Optimzie2ReturnType ret(false, 0.0);

                // is there an improvement
                if(minCutValue + 1e-7 < currentCutValue){
                    

                    this->forEachInternalEdge(nodeLabels, anchorLabel0, anchorLabel1,[&](const uint64_t uLocal, const uint64_t vLocal, const uint64_t edge){
                        if(maxCut_.label(uLocal) == subgraphRes[vLocal]){
                            ufd_.merge(uLocal, vLocal);
                        }
                    });


                    std::unordered_map<uint64_t,uint64_t> mapping;
                    ufd_.representativeLabeling(mapping);

                    for(auto localNode=0; localNode<nLocalNodes_; ++localNode){
                        const auto node = localNodeToGlobal_[localNode];
                        const auto denseLocalLabel =  mapping[ufd_.find(localNode)];
                        nodeLabels[node] = denseLocalLabel + maxNodeLabel + 1;
                    }
                    ret.improvment = true;
                    ret.improvedBy = currentCutValue - minCutValue;


                    // update isDirty 
                    if(ufd_.numberOfSets() <= 2){
                        // set inside to clean
                        // border to dirty
                        for(const auto edge : insideEdges_){
                            // already done
                            // isDirtyEdge_[edge] = false;
                        }
                        for(const auto edge : borderEdges_){
                            isDirtyEdge_[edge] = true;
                        }
                    }
                    else{
                        for(const auto edge : insideEdges_){
                            isDirtyEdge_[edge] = true;
                        }
                        for(const auto edge : borderEdges_){
                            isDirtyEdge_[edge] = true;
                        }
                    }
                
                }
                return ret;  
            }
        private:

            template<class NODE_LABELS>
            uint64_t varMapping(
                const NODE_LABELS & nodeLabels,
                const uint64_t anchorNode0
            ){
                return varMapping(nodeLabels, anchorNode0, anchorNode0);
            }

            template<class NODE_LABELS>
            uint64_t varMapping(
                 const NODE_LABELS & nodeLabels,
                const uint64_t anchorNode0, 
                const uint64_t anchorNode1
            ){

                insideEdges_.clear();
                borderEdges_.clear();

                nLocalNodes_ = 0;
                nLocalEdges_ = 0;
                uint64_t maxNodeLabel = 0;
                const auto anchorLabel0 = nodeLabels[anchorNode0];
                const auto anchorLabel1 = nodeLabels[anchorNode1];
                for(const auto node : graph_.nodes()){
                    const auto nodeLabel = nodeLabels[node];
                    maxNodeLabel = std::max(maxNodeLabel, nodeLabel);
                    if(nodeLabel == anchorLabel0 || nodeLabel == anchorLabel1){

                        globalNodeToLocal_[node] = nLocalNodes_;
                        localNodeToGlobal_[nLocalNodes_] = node;
                        nLocalNodes_ += 1;

                        for(const auto adj : graph_.adjacency(node)){
                            const auto otherNode = adj.node();
                            const auto edge = adj.edge();
                            if(node < otherNode){
                                const auto otherNodeLabel = nodeLabels[otherNode]; 
                                if(otherNodeLabel == anchorLabel0 || otherNodeLabel == anchorLabel1){
                                    nLocalEdges_ += 1;
                                    insideEdges_.push_back(edge);
                                    // mark inside edge as clear
                                    isDirtyEdge_[edge] = false;
                                }
                                // border node
                                else{
                                    borderEdges_.push_back(edge);
                                }
                            }
                        }
                    }
                }
                return maxNodeLabel;
            }

            template<class NODE_LABELS, class F>
            void forEachInternalEdge(const NODE_LABELS & nodeLabels, const uint64_t anchorLabel, F && f){
                for(auto localNode=0; localNode<nLocalNodes_; ++localNode){
                    const auto u = localNodeToGlobal_[localNode];
                    const auto uLocal = globalNodeToLocal_[u];
                    for(const auto adj : graph_.adjacency(u)){
                        const auto v = adj.node();
                        const auto edge = adj.edge();
                        if(u < v  && nodeLabels[v] == anchorLabel){
                            const auto vLocal = globalNodeToLocal_[v];
                            f(uLocal, vLocal, edge);
                        }
                    }
                }
            }

            template<class NODE_LABELS, class F>
            void forEachInternalEdge(
                const NODE_LABELS & nodeLabels, 
                const uint64_t anchorLabel0, 
                const uint64_t anchorLabel1,
                F && f
            ){
                for(auto localNode=0; localNode<nLocalNodes_; ++localNode){
                    const auto u = localNodeToGlobal_[localNode];
                    const auto uLocal = globalNodeToLocal_[u];
                    for(const auto adj : graph_.adjacency(u)){
                        const auto v = adj.node();
                        const auto edge = adj.edge();
                        if(u < v){
                            const auto vLabel = nodeLabels[v];
                            if(vLabel == anchorLabel0 || vLabel == anchorLabel1){
                                const auto vLocal = globalNodeToLocal_[v];
                                f(uLocal, vLocal, edge);
                            }
                        }
                    }
                }
            }


            const ObjectiveType & objective_; 
            const GraphType & graph_;
            const WeightsMapType & weights_;
            GlobalNodeToLocal globalNodeToLocal_;
            LocalNodeToGlobal localNodeToGlobal_;

            uint64_t nLocalNodes_;
            uint64_t nLocalEdges_;

            nifty::max_cut_backend::MaxCutQpbo<float, float> maxCut_;
            nifty::ufd::Ufd<uint64_t> ufd_;
            IsDirtyEdge & isDirtyEdge_;
            std::shared_ptr<SubMcFactoryBase> &  mincutFactory_;

            std::vector<uint64_t> insideEdges_;
            std::vector<uint64_t> borderEdges_;
        };

    }
    /// \endcond




    template<class OBJECTIVE>
    class Cgc : public MulticutBase<OBJECTIVE>
    {
    public: 

        typedef OBJECTIVE Objective;
        typedef OBJECTIVE ObjectiveType;
        typedef typename ObjectiveType::WeightType WeightType;
        typedef MulticutBase<ObjectiveType> Base;
        typedef typename Base::VisitorBase VisitorBase;
        typedef typename Base::VisitorProxy VisitorProxy;
        typedef typename Base::EdgeLabels EdgeLabels;
        typedef typename Base::NodeLabels NodeLabels;
        typedef typename ObjectiveType::Graph Graph;
        typedef typename ObjectiveType::GraphType GraphType;
        typedef typename ObjectiveType::WeightsMap WeightsMap;
        typedef typename GraphType:: template EdgeMap<uint8_t> IsDirtyEdge;

        typedef UndirectedGraph<>                 SubGraph;
        typedef MincutObjective<SubGraph, double> SubObjective;
        typedef MincutFactoryBase<SubObjective>   SubMcFactoryBase;
        typedef MincutBase<SubObjective>          SubMcBase;
        typedef MincutEmptyVisitor<SubObjective>  SubEmptyVisitor;
        typedef typename  SubMcBase::NodeLabels   SubNodeLabels;


        typedef MulticutFactoryBase<Objective>         FactoryBase;
       
    private:
        typedef ComponentsUfd<Graph> Components;
    
    public:

        struct Settings{
            bool doCutPhase{true};
            bool doGlueAndCutPhase{true};
            std::shared_ptr<SubMcFactoryBase> mincutFactory;
        };

        virtual ~Cgc(){
            
        }
        Cgc(const Objective & objective, const Settings & settings = Settings());


        virtual void optimize(NodeLabels & nodeLabels, VisitorBase * visitor);
        virtual const Objective & objective() const;


        virtual const NodeLabels & currentBestNodeLabels( ){
            return *currentBest_;
        }

        virtual std::string name()const{
            return std::string("Cgc");
        }
        virtual void weightsChanged(){ 
        }
        virtual double currentBestEnergy() {
           return currentBestEnergy_;
        }
    private:


        void cutPhase(VisitorProxy & visitorProxy);
        void glueAndCutPhase(VisitorProxy & visitorProxy);

        const Objective & objective_;
        const Graph & graph_;
        const WeightsMap & weights_;

        Components components_;
        Settings settings_;
        IsDirtyEdge isDirtyEdge_;
        detail_cgc::SubmodelOptimizer<Objective> submodel_;
        NodeLabels * currentBest_;
        double currentBestEnergy_;

        


    };

    
    template<class OBJECTIVE>
    Cgc<OBJECTIVE>::
    Cgc(
        const Objective & objective, 
        const Settings & settings
    )
    :   objective_(objective),
        graph_(objective.graph()),
        weights_(objective_.graph()),
        components_(graph_),
        settings_(settings),
        isDirtyEdge_(graph_,true),
        submodel_(objective, isDirtyEdge_,  settings_.mincutFactory),
        currentBest_(nullptr),
        currentBestEnergy_(std::numeric_limits<double>::infinity())
    {

    }


    template<class OBJECTIVE>
    void Cgc<OBJECTIVE>::
    cutPhase(
        VisitorProxy & visitorProxy
    ){

       

        // the node labeling as reference
        auto & nodeLabels = *currentBest_;

        
        // number of components
        const auto nComponents = components_.buildFromLabels(nodeLabels);
        components_.denseRelabeling(nodeLabels);

        // get anchor for each component        
        std::vector<uint64_t> componentsAnchors(nComponents);


        // anchors
        graph_.forEachNode([&](const uint64_t node){
            NIFTY_CHECK_OP(nodeLabels[node],<,nComponents,"");
            componentsAnchors[nodeLabels[node]] = node;
        });

        std::queue<uint64_t> anchorQueue;
        for(const auto & anchor : componentsAnchors){
            anchorQueue.push(anchor);
        }


        // while nothing is on the queue
        visitorProxy.clearLogNames();
        visitorProxy.addLogNames({std::string("QueueSize")});

        while(!anchorQueue.empty()){

            const auto anchorNode = anchorQueue.front();
            anchorQueue.pop();
            const auto anchorLabel = nodeLabels[anchorNode];

            //visitorProxy.printLog(nifty::logging::LogLevel::INFO, "Optimzie1");
            // optimize the submodel 
            const auto ret = submodel_.optimize1(nodeLabels, anchorNode, anchorQueue);
            if(ret.improvment){
                currentBestEnergy_ += ret.minCutValue;
            }

            visitorProxy.setLogValue(0, anchorQueue.size());           
            visitorProxy.visit(this);
        }

        visitorProxy.visit(this);



        visitorProxy.clearLogNames();
    }

    template<class OBJECTIVE>
    void Cgc<OBJECTIVE>::
    glueAndCutPhase(
        VisitorProxy & visitorProxy
    ){


        visitorProxy.clearLogNames();
        visitorProxy.addLogNames({std::string("Sweep")});

        // one anchor for all ``cc-edges``
        typedef std::pair<uint64_t, uint64_t> LabelPair;
        struct LabelPairHash{
        public:
            size_t operator()(const std::pair<uint64_t, uint64_t> & x) const {
                 size_t h = std::hash<uint64_t>()(x.first) ^ std::hash<uint64_t>()(x.second);
                 return h;
            }
        };
        typedef std::unordered_map<LabelPair, uint64_t, LabelPairHash> LabelPairToAnchor;
        LabelPairToAnchor labelPairToAnchorEdge;

        // the node labeling as reference
        auto & nodeLabels = *currentBest_;

        // initially everything is marked as dirty

        auto continueSeach = true;
        auto sweep = 0;
        while(continueSeach){   

            continueSeach = false;


            labelPairToAnchorEdge.clear();
            for(const auto  edge : graph_.edges()){
                const auto u = graph_.u(edge);
                const auto v = graph_.v(edge);
                const auto lu = nodeLabels[std::min(u,v)];
                const auto lv = nodeLabels[std::max(u,v)];
                if(lu != lv){
                    const auto labelPair = LabelPair(lu, lv);
                    labelPairToAnchorEdge.insert(std::make_pair(labelPair, edge));
                }
            }

            for(const auto kv : labelPairToAnchorEdge){
                const auto edge = kv.second;

                if(isDirtyEdge_[edge]){

                    const auto u = graph_.u(edge);
                    const auto v = graph_.v(edge);
                    const auto lu = nodeLabels[u];
                    const auto lv = nodeLabels[v];

                    // if the labels are still different
                    if(lu != lv){
                        const auto ret = submodel_.optimize2(nodeLabels, u, v);
                        if(ret.improvment){
                            continueSeach = true;
                            currentBestEnergy_ -= ret.improvedBy;

                            visitorProxy.setLogValue(0, sweep);           
                            visitorProxy.visit(this);
                        }
                    }
                }
            }
            ++sweep;
        }
    }


    template<class OBJECTIVE>
    void Cgc<OBJECTIVE>::
    optimize(
        NodeLabels & nodeLabels,  VisitorBase * visitor
    ){  

        
        VisitorProxy visitorProxy(visitor);
        //visitorProxy.addLogNames({"violatedConstraints"});

        currentBest_ = &nodeLabels;
        currentBestEnergy_ = objective_.evalNodeLabels(nodeLabels);
        
        visitorProxy.begin(this);

        // the main workhorses
        // cut phase 
        if(settings_.doCutPhase){

            visitorProxy.printLog(nifty::logging::LogLevel::INFO, "Start Cut Phase:");

            this->cutPhase(visitorProxy);
        }
        // glue phase
        if(settings_.doGlueAndCutPhase){
            visitorProxy.printLog(nifty::logging::LogLevel::INFO, "Start Glue & Cut Phase:");
            this->glueAndCutPhase(visitorProxy);
        }


        visitorProxy.end(this);
    }

    template<class OBJECTIVE>
    const typename Cgc<OBJECTIVE>::Objective &
    Cgc<OBJECTIVE>::
    objective()const{
        return objective_;
    }




} // namespace nifty::graph
} // namespace nifty

