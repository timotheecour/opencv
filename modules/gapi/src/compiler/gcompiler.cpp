// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2018 Intel Corporation


#include "precomp.hpp"

#include <vector>
#include <stack>
#include <unordered_map>

#include <ade/util/algorithm.hpp>      // any_of
#include <ade/util/zip_range.hpp>      // zip_range, indexed

#include <ade/graph.hpp>
#include <ade/passes/check_cycles.hpp>

#include "api/gcomputation_priv.hpp"
#include "api/gnode_priv.hpp"   // FIXME: why it is here?
#include "api/gproto_priv.hpp"  // FIXME: why it is here?
#include "api/gcall_priv.hpp"   // FIXME: why it is here?

#include "api/gbackend_priv.hpp" // Backend basic API (newInstance, etc)

#include "compiler/gmodel.hpp"
#include "compiler/gmodelbuilder.hpp"
#include "compiler/gcompiler.hpp"
#include "compiler/gcompiled_priv.hpp"
#include "compiler/passes/passes.hpp"
#include "compiler/passes/pattern_matching.hpp"

#include "executor/gexecutor.hpp"
#include "backends/common/gbackend.hpp"

// <FIXME:>
#if !defined(GAPI_STANDALONE)
#include <opencv2/gapi/cpu/core.hpp>    // Also directly refer to Core
#include <opencv2/gapi/cpu/imgproc.hpp> // ...and Imgproc kernel implementations
#endif // !defined(GAPI_STANDALONE)
// </FIXME:>

#include <opencv2/gapi/gcompoundkernel.hpp> // compound::backend()
#include <opencv2/gapi/render/render.hpp>   // render::ocv::backend()

#include "logger.hpp"

namespace
{
    cv::gapi::GKernelPackage getKernelPackage(cv::GCompileArgs &args)
    {
        auto withAuxKernels = [](const cv::gapi::GKernelPackage& pkg) {
            cv::gapi::GKernelPackage aux_pkg;
            for (const auto &b : pkg.backends()) {
                aux_pkg = combine(aux_pkg, b.priv().auxiliaryKernels());
            }
            return combine(pkg, aux_pkg);
        };

        auto has_use_only = cv::gimpl::getCompileArg<cv::gapi::use_only>(args);
        if (has_use_only)
            return withAuxKernels(has_use_only.value().pkg);

        static auto ocv_pkg =
#if !defined(GAPI_STANDALONE)
            // FIXME add N-arg version combine
            combine(combine(cv::gapi::core::cpu::kernels(),
                    cv::gapi::imgproc::cpu::kernels()),
                    cv::gapi::render::ocv::kernels());
#else
            cv::gapi::GKernelPackage();
#endif // !defined(GAPI_STANDALONE)

        auto user_pkg = cv::gimpl::getCompileArg<cv::gapi::GKernelPackage>(args);
        auto user_pkg_with_aux = withAuxKernels(user_pkg.value_or(cv::gapi::GKernelPackage{}));
        return combine(ocv_pkg, user_pkg_with_aux);
    }

    cv::gapi::GNetPackage getNetworkPackage(cv::GCompileArgs &args)
    {
        return cv::gimpl::getCompileArg<cv::gapi::GNetPackage>(args)
            .value_or(cv::gapi::GNetPackage{});
    }

    cv::util::optional<std::string> getGraphDumpDirectory(cv::GCompileArgs& args)
    {
        auto dump_info = cv::gimpl::getCompileArg<cv::graph_dump_path>(args);
        if (!dump_info.has_value())
        {
            const char* path = std::getenv("GRAPH_DUMP_PATH");
            return path
                ? cv::util::make_optional(std::string(path))
                : cv::util::optional<std::string>();
        }
        else
        {
            return cv::util::make_optional(dump_info.value().m_dump_path);
        }
    }

    template<typename C>
    cv::gapi::GKernelPackage auxKernelsFrom(const C& c) {
        cv::gapi::GKernelPackage result;
        for (const auto &b : c) {
            result = cv::gapi::combine(result, b.priv().auxiliaryKernels());
        }
        return result;
    }

    // Creates ADE graph from input/output proto args
    std::unique_ptr<ade::Graph> makeGraph(const cv::GProtoArgs &ins, const cv::GProtoArgs &outs) {
        std::unique_ptr<ade::Graph> pG(new ade::Graph);
        ade::Graph& g = *pG;

        cv::gimpl::GModel::Graph gm(g);
        cv::gimpl::GModel::init(gm);
        cv::gimpl::GModelBuilder builder(g);
        auto proto_slots = builder.put(ins, outs);

        // Store Computation's protocol in metadata
        cv::gimpl::Protocol p;
        std::tie(p.inputs, p.outputs, p.in_nhs, p.out_nhs) = proto_slots;
        gm.metadata().set(p);

        return pG;
    }

    using adeGraphs = std::vector<std::unique_ptr<ade::Graph>>;

    // Creates ADE graphs (patterns and substitutes) from pkg's transformations
    void makeTransformationGraphs(const cv::gapi::GKernelPackage& pkg,
                                  adeGraphs& patterns,
                                  adeGraphs& substitutes) {
        const auto& transforms = pkg.get_transformations();
        const auto size = transforms.size();
        if (0u == size) return;

        // pre-generate all required graphs
        patterns.resize(size);
        substitutes.resize(size);
        for (auto it : ade::util::zip(ade::util::toRange(transforms),
                                      ade::util::toRange(patterns),
                                      ade::util::toRange(substitutes))) {
            const auto& t = std::get<0>(it);
            auto& p = std::get<1>(it);
            auto& s = std::get<2>(it);

            auto pattern_comp = t.pattern();
            p = makeGraph(pattern_comp.priv().m_ins, pattern_comp.priv().m_outs);

            auto substitute_comp = t.substitute();
            s = makeGraph(substitute_comp.priv().m_ins, substitute_comp.priv().m_outs);
        }
    }

    void checkTransformations(const cv::gapi::GKernelPackage& pkg,
                              const adeGraphs& patterns,
                              const adeGraphs& substitutes) {
        const auto& transforms = pkg.get_transformations();
        const auto size = transforms.size();
        if (0u == size) return;

        GAPI_Assert(size == patterns.size());
        GAPI_Assert(size == substitutes.size());

        const auto empty = [] (const cv::gimpl::SubgraphMatch& m) {
            return m.inputDataNodes.empty() && m.startOpNodes.empty()
                && m.finishOpNodes.empty() && m.outputDataNodes.empty()
                && m.inputTestDataNodes.empty() && m.outputTestDataNodes.empty();
        };

        // **FIXME**: verify other types of endless loops. now, only checking if pattern exists in
        //            substitute within __the same__ transformation
        for (size_t i = 0; i < size; ++i) {
            const auto& p = patterns[i];
            const auto& s = substitutes[i];

            auto matchInSubstitute = cv::gimpl::findMatches(*p, *s);
            if (!empty(matchInSubstitute)) {
                std::stringstream ss;
                ss << "Error: (in transformation with description: '"
                    << transforms[i].description
                    << "') pattern is detected inside substitute";
                throw std::runtime_error(ss.str());
            }
        }
    }
} // anonymous namespace


// GCompiler implementation ////////////////////////////////////////////////////

cv::gimpl::GCompiler::GCompiler(const cv::GComputation &c,
                                GMetaArgs              &&metas,
                                GCompileArgs           &&args)
    : m_c(c), m_metas(std::move(metas)), m_args(std::move(args))
{
    using namespace std::placeholders;

    auto kernels_to_use  = getKernelPackage(m_args);
    auto networks_to_use = getNetworkPackage(m_args);
    std::unordered_set<cv::gapi::GBackend> all_backends;
    const auto take = [&](std::vector<cv::gapi::GBackend> &&v) {
        all_backends.insert(v.begin(), v.end());
    };
    take(kernels_to_use.backends());
    take(networks_to_use.backends());

    m_all_kernels = cv::gapi::combine(kernels_to_use,
                                      auxKernelsFrom(all_backends));
    // NB: The expectation in the line above is that
    // NN backends (present here via network package) always add their
    // inference kernels via auxiliary...()

    // sanity check transformations
    {
        adeGraphs patterns, substitutes;
        // FIXME: returning vectors of unique_ptrs from makeTransformationGraphs results in
        //        compile error (at least) on GCC 9 with usage of copy-ctor of std::unique_ptr, so
        //        using initialization by lvalue reference instead
        makeTransformationGraphs(m_all_kernels, patterns, substitutes);
        checkTransformations(m_all_kernels, patterns, substitutes);

        // NB: saving generated patterns to m_all_patterns to be used later in passes
        m_all_patterns = std::move(patterns);
    }

    auto dump_path       = getGraphDumpDirectory(m_args);

    m_e.addPassStage("init");
    m_e.addPass("init", "check_cycles",  ade::passes::CheckCycles());
    m_e.addPass("init", "apply_transformations",
                std::bind(passes::applyTransformations, _1, std::cref(m_all_kernels),
                    std::cref(m_all_patterns)));  // Note: and re-using patterns here
    m_e.addPass("init", "expand_kernels",
                std::bind(passes::expandKernels, _1,
                          m_all_kernels)); // NB: package is copied
    m_e.addPass("init", "topo_sort",     ade::passes::TopologicalSort());
    m_e.addPass("init", "init_islands",  passes::initIslands);
    m_e.addPass("init", "check_islands", passes::checkIslands);
    // TODO:
    // - Check basic graph validity (i.e., all inputs are connected)
    // - Complex dependencies (i.e. parent-child) unrolling
    // - etc, etc, etc

    // Remove GCompoundBackend to avoid calling setupBackend() with it in the list
    m_all_kernels.remove(cv::gapi::compound::backend());

    m_e.addPassStage("kernels");
    m_e.addPass("kernels", "bind_net_params",
                std::bind(passes::bindNetParams, _1,
                          networks_to_use));
    m_e.addPass("kernels", "resolve_kernels",
                std::bind(passes::resolveKernels, _1,
                          std::ref(m_all_kernels)));  // NB: and not copied here
                                                      // (no compound backend present here)
    m_e.addPass("kernels", "check_islands_content", passes::checkIslandsContent);

    m_e.addPassStage("meta");
    m_e.addPass("meta", "initialize",   std::bind(passes::initMeta, _1, std::ref(m_metas)));
    m_e.addPass("meta", "propagate",    std::bind(passes::inferMeta, _1, false));
    m_e.addPass("meta", "finalize",     passes::storeResultingMeta);
    // moved to another stage, FIXME: two dumps?
    //    m_e.addPass("meta", "dump_dot",     passes::dumpDotStdout);

    // Special stage for backend-specific transformations
    // FIXME: document passes hierarchy and order for backend developers
    m_e.addPassStage("transform");

    m_e.addPassStage("exec");
    m_e.addPass("exec", "fuse_islands",     passes::fuseIslands);
    m_e.addPass("exec", "sync_islands",     passes::syncIslandTags);

    if (dump_path.has_value())
    {
        m_e.addPass("exec", "dump_dot", std::bind(passes::dumpGraph, _1,
                                                  dump_path.value()));
    }

    // FIXME: This should be called for "ActiveBackends" only (see metadata).
    // However, ActiveBackends are known only after passes are actually executed.
    // At these stage, they are not executed yet.
    ade::ExecutionEngineSetupContext ectx(m_e);
    auto backends = m_all_kernels.backends();
    for (auto &b : backends)
    {
        b.priv().addBackendPasses(ectx);
    }
}

void cv::gimpl::GCompiler::validateInputMeta()
{
    if (m_metas.size() != m_c.priv().m_ins.size())
    {
        util::throw_error(std::logic_error
                    ("COMPILE: GComputation interface / metadata mismatch! "
                     "(expected " + std::to_string(m_c.priv().m_ins.size()) + ", "
                     "got " + std::to_string(m_metas.size()) + " meta arguments)"));
    }

    const auto meta_matches = [](const GMetaArg &meta, const GProtoArg &proto) {
        switch (proto.index())
        {
        // FIXME: Auto-generate methods like this from traits:
        case GProtoArg::index_of<cv::GMat>():
        case GProtoArg::index_of<cv::GMatP>():
            return util::holds_alternative<cv::GMatDesc>(meta);

        case GProtoArg::index_of<cv::GScalar>():
            return util::holds_alternative<cv::GScalarDesc>(meta);

        case GProtoArg::index_of<cv::detail::GArrayU>():
            return util::holds_alternative<cv::GArrayDesc>(meta);

        default:
            GAPI_Assert(false);
        }
        return false; // should never happen
    };

    for (const auto &meta_arg_idx : ade::util::indexed(ade::util::zip(m_metas, m_c.priv().m_ins)))
    {
        const auto &meta  = std::get<0>(ade::util::value(meta_arg_idx));
        const auto &proto = std::get<1>(ade::util::value(meta_arg_idx));

        if (!meta_matches(meta, proto))
        {
            const auto index  = ade::util::index(meta_arg_idx);
            util::throw_error(std::logic_error
                        ("GComputation object type / metadata descriptor mismatch "
                         "(argument " + std::to_string(index) + ")"));
            // FIXME: report what we've got and what we've expected
        }
    }
    // All checks are ok
}

void cv::gimpl::GCompiler::validateOutProtoArgs()
{
    for (const auto &out_pos : ade::util::indexed(m_c.priv().m_outs))
    {
        const auto &node = proto::origin_of(ade::util::value(out_pos)).node;
        if (node.shape() != cv::GNode::NodeShape::CALL)
        {
            auto pos = ade::util::index(out_pos);
            util::throw_error(std::logic_error
                        ("Computation output " + std::to_string(pos) +
                         " is not a result of any operation"));
        }
    }
}

cv::gimpl::GCompiler::GPtr cv::gimpl::GCompiler::generateGraph()
{
    validateInputMeta();
    validateOutProtoArgs();
    return makeGraph(m_c.priv().m_ins, m_c.priv().m_outs);
}

void cv::gimpl::GCompiler::runPasses(ade::Graph &g)
{
    m_e.runPasses(g);
    GAPI_LOG_INFO(NULL, "All compiler passes are successful");
}

void cv::gimpl::GCompiler::compileIslands(ade::Graph &g)
{
    GModel::Graph gm(g);
    std::shared_ptr<ade::Graph> gptr(gm.metadata().get<IslandModel>().model);
    GIslandModel::Graph gim(*gptr);

    // Run topological sort on GIslandModel first
    auto pass_ctx = ade::passes::PassContext{*gptr};
    ade::passes::TopologicalSort{}(pass_ctx);

    // Now compile islands
    GIslandModel::compileIslands(gim, g, m_args);
}

cv::GCompiled cv::gimpl::GCompiler::produceCompiled(GPtr &&pg)
{
    // This is the final compilation step. Here:
    // - An instance of GExecutor is created. Depending on the platform,
    //   build configuration, etc, a GExecutor may be:
    //   - a naive single-thread graph interpreter;
    //   - a std::thread-based thing
    //   - a TBB-based thing, etc.
    // - All this stuff is wrapped into a GCompiled object and returned
    //   to user.

    // Note: this happens in the last pass ("compile_islands"):
    // - Each GIsland of GIslandModel instantiates its own,
    //   backend-specific executable object
    //   - Every backend gets a subgraph to execute, and builds
    //     an execution plan for it (backend-specific execution)
    // ...before call to produceCompiled();

    const auto &outMetas = GModel::ConstGraph(*pg).metadata()
        .get<OutputMeta>().outMeta;
    std::unique_ptr<GExecutor> pE(new GExecutor(std::move(pg)));
    // FIXME: select which executor will be actually used,
    // make GExecutor abstract.

    GCompiled compiled;
    compiled.priv().setup(m_metas, outMetas, std::move(pE));
    return compiled;
}

cv::GCompiled cv::gimpl::GCompiler::compile()
{
    std::unique_ptr<ade::Graph> pG = generateGraph();
    runPasses(*pG);
    compileIslands(*pG);
    return produceCompiled(std::move(pG));
}
