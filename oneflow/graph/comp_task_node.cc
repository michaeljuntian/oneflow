#include "graph/comp_task_node.h"
#include "operator/operator_factory.h"
#include "operator/clone_op.h"
#include "path/path.h"

namespace oneflow {

void CompTaskNode::SubscribeRegstDescInnerPath() {
  if (IsFwNode()) { return; }
  TaskNode* fw_node = GetFwNode();
  Subscribe(fw_node->GetProducedRegstDesc("data"));
  Subscribe(fw_node->GetProducedRegstDesc("model_tmp"));
  TaskNode::SubscribeRegstDescInnerPath();
}

void CompTaskNode::FwBuildExecAndProducedRegsts(Path* path) {
  (this->*(path->Func4FwBuildExecAndProducedRegsts()))(path);
}

void CompTaskNode::DataFwBuildExecAndProducedRegsts(Path* path) {
  Lbn2NodeObnMap lbn2producer;
  Lbn2NodeIbnVecMap extern_in_lbn2consumers;
  FwBuildFromUserOps(&lbn2producer, &extern_in_lbn2consumers);
  if (GetBpNode() != nullptr) {
    FwAddCopyInOp(&extern_in_lbn2consumers);
  }
  mut_exec_gph().UpdateSourceAndSink();
  // data regst
  std::unique_ptr<RegstDesc> data_regst(new DisContigRegstDesc);
  BindProducedRegstAndOutEdge(data_regst.get(), SoleOutEdge());
  EnrollProducedRegstDesc("data", std::move(data_regst));
  FwSetDataRegstDesc(lbn2producer, extern_in_lbn2consumers);
  // model_tmp regst
  std::unique_ptr<RegstDesc> model_tmp_regst(new DisContigRegstDesc);
  EnrollProducedRegstDesc("model_tmp", std::move(model_tmp_regst));
  FwSetModelTmpRegstDesc();
}

void CompTaskNode::ModelUpdateFwBuildExecAndProducedRegsts(Path* path) {
  TODO();
  /*if (IsFaker()) {
    CompTaskNode* mccoy = path->Faker2Mccoy(this);
    RegstDesc* regst = mccoy->GetProducedRegstDesc("model_diff");
    BindProducedRegstAndOutEdge(regst, SoleOutEdge());
    return;
  }
  std::unique_ptr<RegstDesc> model_regst(new ContigRegstDesc);
  ExecNode* exec_node = mut_exec_gph().NewFinalNode();
  exec_node->mut_op() = chain_node()->op_vec().front();
  mut_exec_gph().UpdateSourceAndSink();
  for (std::shared_ptr<const Operator> op : path->GetDataChain()->op_vec()) {
    for (const std::string& mbn : op->model_bns()) {
      std::string lbn = op->mbn2lbn(mbn);
      exec_node->AddConsumedLbnRegstPair(lbn, GetRelatedRegst(SoleInEdge()));
      exec_node->AddProducedLbnRegstPair(lbn, model_regst.get());
    }
  }
  EnrollProducedRegstDesc("model", std::move(model_regst));
  AddInPathLbn2ProducedRegst();
  */
}

void CompTaskNode::ModelLoadFwBuildExecAndProducedRegsts(Path*) {
  TODO();
}

void CompTaskNode::ModelSaveFwBuildExecAndProducedRegsts(Path*) {
  TODO();
}

void CompTaskNode::FwBuildFromUserOps(
    Lbn2NodeObnMap* lbn2producer,
    Lbn2NodeIbnVecMap* extern_in_lbn2consumers) {
  for (std::shared_ptr<const Operator> op : chain_node()->op_vec()) {
    ExecNode* cur_node = mut_exec_gph().NewFinalNode();
    cur_node->mut_op() = op;
    for (const std::string& obn : op->output_bns()) {
      std::string lbn = op->obn2lbn(obn);
      CHECK(lbn2producer->emplace(lbn, {cur_node, obn}).second);
    }
  }
  for (const std::unique_ptr<ExecNode>& cur_node : exec_gph().nodes()) {
    for (const std::string& ibn : cur_node->op()->input_bns()) {
      std::string lbn = cur_node->op()->ibn2lbn(ibn);
      auto producer_it = lbn2producer->find(lbn);
      if (producer_it != lbn2producer->end()) {
        ExecEdge* edge = mut_exec_gph().NewExecEdge();
        edge->set_lbn(lbn);
        edge->mut_src_bn() = producer_it->second.second;
        edge->mut_dst_bn() = ibn;
        Connect(producer_it->second.first, edge, cur_node.get());
      } else {
        (*extern_in_lbn2consumers)[lbn].push_back({cur_node.get(), ibn});
      }
    }
  }
}

void CompTaskNode::FwAddCopyInOp(Lbn2NodeIbnVecMap* extern_in_lbn2consumers) {
  if (extern_in_lbn2consumers->empty()) { return; }
  // Construct Copy Operator
  OperatorConf pb_op_conf;
  pb_op_conf.set_name("copy_in_" + std::to_string(node_id()));
  pb_op_conf.mutable_copy_op_conf()->set_copy_type(CopyInOpType());
  for (const auto& pair : *extern_in_lbn2consumers) {
    pb_op_conf.mutable_copy_op_conf()->add_copied_lbns(pair.first);
  }
  std::shared_ptr<const Operator> copy_op = ConstructOpFromPbConf(pb_op_conf);
  // Construct Exec Node
  ExecNode* copy_node = mut_exec_gph().NewFinalNode();
  copy_node->mut_op() = copy_op;
  // Connect CopyNode and OldConsumer
  for (const std::string& obn : copy_node->op()->output_bns()) {
    std::string lbn = copy_node->op()->obn2lbn(obn);
    const auto& old_consumers = extern_in_lbn2consumers->at(lbn);
    for (std::pair<ExecNode*, std::string> old_consumer : old_consumers) {
      ExecEdge* edge = mut_exec_gph().NewExecEdge();
      edge->set_lbn(lbn);
      edge->mut_dst_bn() = old_consumer.second;
      edge->mut_src_bn() = obn;
      Connect(copy_node, edge, old_consumer->first);
    }
  }
  for (const auto& pair : *extern_in_lbn2consumers) {
    extern_in_lbn2consumers->at(lbn) = {copy_node};
  }
}

void CompTaskNode::FwSetDataRegstDesc(
    const Lbn2NodeObnMap& lbn2producer,
    const Lbn2NodeIbnVecMap& extern_in_lbn2consumers) {
  RegstDesc* in_regst = GetRelatedRegst(SoleInEdge());
  RegstDesc* out_regst = GetRelatedRegst(SoleOutEdge());
  // blob on exec_edge
  for (const std::unique_ptr<ExecEdge>& edge : exec_gph().edges()) {
    Shape* ptr = out_regst->EnrollWithPbnAndLbn(edge->pbn(), edge->lbn());
    // src
    edge->src_node()->BindBnInOpAndRegst(edge->src_bn(), out_regst);
    edge->src_node()->op()->SetShapePtr(edge->src_bn(), ptr);
    // dst
    edge->dst_node()->BindBnInOpAndRegst(edge->dst_bn(), out_regst);
    edge->dst_node()->op()->SetShapePtr(edge->dst_bn(), ptr);
  }
  // extern in blobs
  for (const auto& pair : extern_in_lbn2consumers) {
    const std::string& lbn = pair.first;
    Shape* ptr = in_regst->GetShapePtrFromLbn(lbn);
    for (std::pair<ExecNode*, std::string> consumer : pair.second) {
      ExecNode* node = consumer.first;
      const std::string& ibn = consumer.second;
      node->op()->SetShapePtr(ibn, ptr);
      node->BindBnInOpAndRegst(ibn, in_regst);
    }
  }
  // extern out blobs
  for (const std::string& lbn : chain_node()->output_lbns()) {
    const std::pair<ExecNode*, std::string>& producer = lbn2producer.at(lbn);
    ExecNode* node = producer.first;
    CHECK(exec_gph().IsLastNode(node));
    const std::string& obn = producer.second;
    Shape* ptr = out_regst->EnrollWithLbn(lbn);
    node->op()->SetShapePtr(obn, ptr);
    node->BindBnInOpAndRegst(obn, out_regst);
  }
  // data tmp blobs
  for (const std::unique_ptr<ExecNode>& node : exec_gph().nodes()) {
    for (const std::string& dtbn : node->op()->data_tmp_bns()) {
      std::string lbn = node->op()->dtbn2lbn(dtbn);
      Shape* ptr = data_regst->EnrollWithLbn(lbn);
      node->op()->SetShapePtr(dtbn, ptr);
      node->BindBnInOpAndRegst(dtbn, out_regst);
    }
  }
  // Inference Shape
  for (const ExecNode& node : exec_gph()) {
    node->op()->InferShape4ObAndDtbFromIb();
  }
}

void CompTaskNode::FwSetModelTmpRegstDesc() {
  RegstDesc* model_tmp_regst = GetProducedRegstDesc("model_tmp");
  for (const std::unique_ptr<ExecNode>& node : exec_gph().nodes()) {
    for (const std::string& mtbn : node->op()->model_tmp_bns()) {
      std::string lbn = node->op()->mtbn2lbn(mtbn);
      Shape* ptr = model_tmp_regst->EnrollWithLbn(lbn);
      node->op()->SetShapePtr(mtbn, ptr);
      node->BindBnInOpAndRegst(mtbn, model_tmp_regst);
    }
    node->op()->InferShape4Mtb();
  }
}

void CompTaskNode::BpBuildExecAndProducedRegsts(Path* path) {
  const ExecGraph& fw_gph = GetFwNode()->exec_gph();
  const ExecNode* cp_in_node = fw_gph.source_node().SoleOutEdge()->dst_node();
  HashMap<const ExecNode*, ExecNode*> fw_node2bp_node;
  HashMap<ExecEdge*, const ExecEdge*> bp_edge2fw_edge;
  BpBuildExecGraph(fw_gph, cp_in_node, &fw_node2bp_node, &bp_edge2fw_edge);
  //
  std::unique_ptr<RegstDesc> data_diff_regst(new DisContigRegstDesc);
  BindProducedRegstAndOutEdge(data_diff_regst.get(), SoleOutEdge());
  EnrollProducedRegstDesc("data_diff", std::move(data_diff_regst));
  BpSetDataDiffRegst(cp_in_node, fw_node2bp_node, bp_edge2fw_edge);
  //
  std::unique_ptr<RegstDesc> model_diff_regst(new ContigRegstDesc);
  EnrollProducedRegstDesc("model_diff", std::move(model_diff_regst));
  BpSetModelDiffRegst();
}

void CompTaskNode::BpBuildExecGraph(
    const ExecGraph& fw_gph,
    const ExecNode* cp_in_node,
    HashMap<const ExecNode*, ExecNode*>* fw_node2bp_node,
    HashMap<ExecEdge*, const ExecEdge*>* bp_edge2fw_edge) {
  for (const std::unique_ptr<ExecNode>& fw_node : fw_gph.nodes()) {
    if (fw_node.get() == cp_in_node) { continue; }
    ExecNode* bp_node = mut_exec_gph().NewFinalNode();
    bp_node->mut_op() = fw_node->op();
    fw_node2bp_node->emplace(fw_node.get(), bp_node);
  }
  for (const std::unique_ptr<ExecEdge>& fw_edge : fw_gph.edges()) {
    if (fw_edge->src_node() == cp_in_node) { continue; }
    ExecEdge* bp_edge = mut_exec_gph().NewExecEdge();
    bp_edge->set_lbn(fw_edge->lbn());
    bp_edge->mut_src_bn() = fw_edge->dst_bn();
    bp_edge->mut_dst_bn() = fw_edge->src_bn();
    Connect(fw_node2bp_node->at(fw_edge->dst_node()),
            bp_edge,
            fw_node2bp_node->at(fw_edge->src_node()));
    CHECK(bp_edge2fw_edge->emplace(bp_edge, fw_edge.get()).second);
  }
}

void CompTaskNode::BpSetDataDiffRegst(
    const ExecNode* cp_in_node,
    const HashMap<const ExecNode*, ExecNode*>& fw_node2bp_node,
    const HashMap<ExecEdge*, const ExecEdge*>& bp_edge2fw_edge) {
  // Regsts
  RegstDesc* in_diff_regst = GetRelatedRegst(SoleOutEdge());
  RegstDesc* out_diff_regst = GetRelatedRegst(SoleInEdge());
  RegstDesc* in_regst = GetRelatedRegst(GetFwNode()->SoleInEdge());
  RegstDesc* out_regst = GetRelatedRegst(GetFwNode()->SoleOutEdge());
  RegstDesc* model_tmp_regst = GetFwNode()->GetProducedRegstDesc("model_tmp");
  // blobs on edge 
  for (const std::unique_ptr<ExecEdge>& edge : exec_gph().edges()) {
    Shape* ptr = in_diff_regst->EnrollWithPbnAndLbn(edge->pbn(), edge->lbn());
    edge->src_node()->op()->SetShapePtr(edge->src_bn(), ptr);
    edge->dst_node()->op()->SetShapePtr(edge->dst_bn(), ptr);
  }
  // extern in blobs
  for (const std::unique_ptr<ExecNode>& bp_node : exec_gph().nodes()) {
    std::unordered_set<std::string> found_bns;
    for (ExecEdge* edge : bp_node->in_edges()) {
      found_bns.insert(edge->dst_bn());
    }
    for (const auto& obn : bp_node->op()->output_bns()) {
      if (found_bns.find(obn) != found_lbns.end()) { continue; }
      std::string lbn = bp_node->op()->obn2lbn(obn);
      bp_node()->op()->SetShapePtr(obn, in_regst->GetShapePtrFromLbn(lbn));
    }
  }
  // extern out blobs
  for (ExecEdge* edge : cp_in_node->out_edges()) {
    ExecNode* bp_node = fw_node2bp_node.at(edge->dst_node());
    Shape* ptr = out_regst->GetShapePtrFromLbn(edge->lbn());
    bp_node->op()->SetShapePtr(edge->dst_bn(), ptr);
  }
  // data tmp blobs
  for (const std::unique_ptr<ExecNode>& node : exec_gph().nodes()) {
    for (const std::string& dtbn : node->op()->data_tmp_bns()) {
      std::string lbn = node->op()->dtbn2lbn(dtbn);
      Shape* ptr = data_diff_regst->EnrollWithLbn(lbn);
      node->op()->SetShapePtr(dtbn, ptr);
    }
  }
  // Enroll
  EnrollProducedRegstDesc("data_diff", std::move(out_regst));
}

void CompTaskNode::BpSetProducedRegstDescs() {
  RegstDesc* data_diff_regst = GetRelatedRegst(SoleOutEdge());
  for (const std::unique_ptr<ExecNode>& cur_node : exec_gph().nodes()) {
    for (const std::string& mbn : cur_node->op()->model_bns()) {
      std::string lbn = cur_node->op()->mbn2lbn(mbn);
      model_diff_regst->EnrollWithLbn(lbn);
    }
    for (const std::string& mtbn : cur_node->op()->model_tmp_bns()) {
      std::string lbn = cur_node->op()->mtbn2lbn(mtbn);
      std::string pbn = cur_node->lbn2pbn(lbn);
      model_tmp_regst->EnrollWithPbnAndLbn(pbn, lbn);
    }
  }
  AddInPathLbn2ProducedRegst();
  EnrollProducedRegstDesc("model_diff", std::move(model_diff_regst));
  EnrollProducedRegstDesc("model_tmp", std::move(model_tmp_regst));
}

void SortByParallelId(std::vector<CompTaskNode*>* comp_node_vec) {
  std::sort(comp_node_vec->begin(), comp_node_vec->end(), []
      (const CompTaskNode* lhs, const CompTaskNode* rhs) {
    return lhs->parallel_id() < rhs->parallel_id();
  });
}

} // namespace oneflow
