#include <algorithm>

#include "AutoSchedule.h"
#include "AutoScheduleUtils.h"
#include "ExprUsesVar.h"
#include "FindCalls.h"
#include "Func.h"
#include "ParallelRVar.h"
#include "RealizationOrder.h"
#include "RegionCosts.h"
#include "Scope.h"
#include "Simplify.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;
using std::map;
using std::set;
using std::deque;
using std::pair;
using std::make_pair;

namespace {

/** Helper function to simplify the upper and lower bounds
 * of each dimension of a box.*/
void simplify_box(Box &b) {
    for (size_t i = 0; i < b.size(); i++) {
        b[i].min = simplify(b[i].min);
        b[i].max = simplify(b[i].max);
    }
}

/** Helper function to merge the partial region map into the result
 * region map. */
void merge_regions(map<string, Box> &result, map<string, Box> &partial) {
    // Merge regions from 'partial' with an existing region in 'result'.
    for (const auto &reg : partial) {
        auto iter = result.find(reg.first);
        if (iter == result.end()) {
            result.emplace(reg.first, reg.second);
        } else {
            merge_boxes(iter->second, reg.second);
        }
    }
}

/** Representation of a function stage in the pipeline. */
struct FStage {
    Function func;
    uint32_t stage_num;
    FStage(Function func, uint32_t stage_num) : func(func), stage_num(stage_num) {}

    bool operator==(const FStage &other_stage) const {
        return (func.name() == other_stage.func.name()) &&
               (stage_num == other_stage.stage_num);
    }

    bool operator<(const FStage &other_stage) const {
        return func.name() < other_stage.func.name() ||
               ((func.name() == other_stage.func.name()) &&
                (stage_num < other_stage.stage_num));
    }

    friend std::ostream& operator<<(std::ostream &stream, const FStage &s) {
        stream << "(" << s.func.name() << ", " << s.stage_num << ")";
        return stream;
    }
};

/** Helper function to set the compute and store level of all function
 * stages in the environment as root. */
void set_schedule_defaults(map<string, Function> &env) {
    for (auto &kv : env) {
        kv.second.schedule().store_level() = LoopLevel::root();
        kv.second.schedule().compute_level() = LoopLevel::root();

        // Set the schedule for each update definition.
        for (size_t u = 0; u < kv.second.updates().size(); u++) {
            kv.second.update_schedule(u).store_level() = LoopLevel::root();
            kv.second.update_schedule(u).compute_level() = LoopLevel::root();
        }
    }
}

/** Return true if all the pipeline outputs have estimates specified
 * on each of their dimensions. */
bool check_estimates_on_outputs(const vector<Function> &outputs) {
    bool estimates_avail = true;
    for (const auto &out : outputs) {
        const vector<Bound> &estimates = out.schedule().estimates();
        if (estimates.size() != out.args().size()) {
            estimates_avail = false;
            break;
        }
        const vector<string> &vars = out.args();
        // Check if the estimate for each dimension is available and it is an integer.
        for (uint32_t i = 0; i < estimates.size(); i++) {
            if ((std::find(vars.begin(), vars.end(), estimates[i].var) == vars.end()) ||
                !(estimates[i].min.as<IntImm>() && estimates[i].extent.as<IntImm>())) {
                estimates_avail = false;
                break;
            }
        }
    }
    return estimates_avail;
}

struct DependenceAnalysis {
    // Map containing all the functions in the pipeline.
    const map<string, Function> &env;
    const FuncValueBounds &func_val_bounds;

    /** TODO: Auto scheduling for large benchmarks is bottlenecked by the bound inference.
     * Bound queries with the same parameters are common during the grouping process;
     * it might be beneficial to build a cache for bounds queries. */

    DependenceAnalysis(const map<string, Function> &env, const FuncValueBounds &func_val_bounds)
        : env(env), func_val_bounds(func_val_bounds) {}

    /** Return the regions of the producers ('prods') required to compute the region
     * of the function stage ('f', 'stage_num') specified by 'bounds'. When
     * 'only_regions_computed' is set to true, this only returns the computed
     * regions and not the total allocated regions.
     */
    map<string, Box> regions_required(Function f, int stage_num,
                                      const DimBounds &bounds,
                                      const set<string> &prods,
                                      bool only_regions_computed);

    /** Return the regions of the producers ('prods') required to compute the region
     * of the function specified by 'pure_bounds'. When 'only_regions_computed'
     * is set to true, this only returns the computed regions and not the total
     * allocated regions.
     */
    map<string, Box> regions_required(Function f,
                                      const DimBounds &pure_bounds,
                                      const set<string> &prods,
                                      bool only_regions_computed);

    /** Return redundantly computed regions of producers ('prods') while computing
     * a region of the function stage ('f', 'stage_num') specified by 'bounds'.
     * 'var' is the dimension along which redundant computation is accounted for.
     * When 'only_regions_computed' is set to true, this only returns the computed
     * regions and not the total allocated regions. When 'only_regions_computed'
     * is set to true, this only returns the computed regions and not the total
     * allocated regions.
     */
    map<string, Box> redundant_regions(Function f, int stage_num, string var,
                                       const DimBounds &bounds,
                                       const set<string> &prods,
                                       bool only_regions_computed);

    /** Return overlapping regions of producers ('prods') while computing a function
     * stage along each of the dimensions.
     */
    vector<map<string, Box>>
    overlap_regions(Function f, int stage_num, const DimBounds &bounds,
                    const set<string> &prods, bool only_regions_computed);
};

/** Return the regions of the producers ('prods') required to compute the region
 * of the function specified by 'pure_bounds'. */
map<string, Box>
DependenceAnalysis::regions_required(Function f, const DimBounds &pure_bounds,
                                     const set<string> &prods,
                                     bool only_regions_computed) {
    // Find the regions required for each stage and merge them.
    map<string, Box> regions;
    int num_stages = f.updates().size() + 1;
    for (int s = 0; s < num_stages; s++) {
        DimBounds bounds = get_stage_bounds(f, s, pure_bounds);
        map<string, Box> stage_regions =
            regions_required(f, s, bounds, prods, only_regions_computed);

        merge_regions(regions, stage_regions);
    }
    return regions;
}

/** Helper function to queue regions that need to be traversed.
 * 'f_queue' is the queue into which the regions specified by
 * 'prod_func' and 'region' will be added. */
void queue_func_regions(deque<pair<FStage, DimBounds>> &f_queue,
                        const Function &prod_func, const Box &region) {
    DimBounds prod_pure_bounds;
    const vector<string> &args = prod_func.args();

    internal_assert(region.size() == args.size());

    // The region only specifies the extent of each dimension
    // by position. Populating a map which is keyed by name.
    for (size_t v = 0; v < args.size(); v++) {
        prod_pure_bounds[args[v]] = region[v];
    }

    // Get the bounds of all stages in a function from the
    // bounds on the pure dimensions.
    vector<DimBounds> prod_bounds = get_stage_bounds(prod_func, prod_pure_bounds);

    size_t num_stages = prod_func.updates().size() + 1;

    internal_assert(prod_bounds.size() == num_stages);

    // Add all stages of a function into the queue.
    for (size_t prod_s = 0; prod_s < num_stages; prod_s++) {
        FStage prod_stage(prod_func, prod_s);
        f_queue.push_back(make_pair(prod_stage, prod_bounds[prod_s]));
    }
}

/** Helper function for merging 'curr_regions' to the global map of regions
 * and adding them to the queue of regions that need to be traversed.
 * 'prods' is the set of producer functions that are under consideration. */
void merge_and_queue_regions(deque<pair<FStage, DimBounds>> &f_queue,
                             map<string, Box> &regions,
                             map<string, Box> &curr_regions,
                             const set<string> &prods,
                             const map<string, Function> &env,
                             bool only_regions_computed,
                             string curr_func_name) {
    for (const auto &reg : curr_regions) {
        // Merge region with an existing region of a function in the
        // global map. Do not merge the parent function itself to the region
        // when querying only for the values computed.
        if (!only_regions_computed || (only_regions_computed && (reg.first != curr_func_name))) {
            auto iter = regions.find(reg.first);
            if (iter == regions.end()) {
                regions.emplace(reg.first, reg.second);
            } else {
                merge_boxes(iter->second, reg.second);
            }
        }

        // Skip adding the current region into to the queue if the function
        // is not in 'prods'.
        if (prods.find(reg.first) == prods.end()) {
            continue;
        }

        const auto &it = env.find(reg.first);
        if ((it != env.end()) && (reg.first != curr_func_name)) {
            // Add all stages of the function representing the
            // region into the queue.
            queue_func_regions(f_queue, it->second, reg.second);
        }
    }
}

/** Return the regions of the producers ('prods') required to compute the region
 * of the function stage ('f', 'stage_num') specified by 'bounds'. */
map<string, Box>
DependenceAnalysis::regions_required(Function f, int stage_num,
                                     const DimBounds &bounds,
                                     const set<string> &prods,
                                     bool only_regions_computed) {
    // Iteratively compute the required regions by traversing the chain
    // of dependencies.

    // Map of all the required regions.
    map<string, Box> regions;
    deque<pair<FStage, DimBounds>> f_queue;

    // Add the query function and its region to the queue
    FStage start(f, stage_num);
    f_queue.push_back(make_pair(start, bounds));

    while(!f_queue.empty()) {

        FStage s = f_queue.front().first;
        DimBounds curr_bounds = f_queue.front().second;

        Definition def = get_stage_definition(s.func, s.stage_num);
        // Scope for containing all the estimates on parameters and intervals.
        Scope<Interval> curr_scope;

        const vector<Dim> &dims = def.schedule().dims();

        // Substitute parameter estimates into the bounds and add them to the
        // current scope.
        for (int d = 0; d < (int)dims.size() - 1; d++) { // Ignore '__outermost'
            string var_name = dims[d].var;
            internal_assert(curr_bounds.find(var_name) != curr_bounds.end());

            Expr lower = SubstituteVarEstimates().mutate(get_element(curr_bounds, dims[d].var).min);
            Expr upper = SubstituteVarEstimates().mutate(get_element(curr_bounds, dims[d].var).max);
            Interval simple_bounds = Interval(simplify(lower), simplify(upper));
            curr_scope.push(var_name, simple_bounds);
        }

        // If the function has an extern definition, there is no visibility into
        // the expression defining the function. So the regions required will be
        // the entire domain of the inputs to the extern func. Use the estimates
        // on the inputs to the extern function if available.
        //
        // TODO: Query the extern function for bounds of the functions which it
        // it depends on. This can be done by calling the extern func in the
        // bounds query mode.
        if (s.func.has_extern_definition()) {
            for (const ExternFuncArgument &arg : s.func.extern_arguments()) {
                if (arg.is_func()) {
                    // If the argument is an entire function, the bounds of the
                    // function required are unknown. Create an infinite region
                    // of the correct dimension, update the region map, and
                    // add it to the queue.
                    string prod_name = Function(arg.func).name();
                    const Function &prod_func = get_element(env, prod_name);
                    map<string, Box> prod_reg;
                    const vector<string> &args = prod_func.args();
                    for (size_t v = 0; v < args.size(); v++) {
                        prod_reg[prod_name].push_back(Interval());
                    }
                    merge_and_queue_regions(f_queue, regions, prod_reg, prods,
                                            env, only_regions_computed, s.func.name());
                } else if (arg.is_expr()) {
                    // Find the boxes required for the expression and add the regions
                    // to the queue.
                    Expr subs_arg = SubstituteVarEstimates().mutate(arg.expr);
                    map<string, Box> arg_regions = boxes_required(subs_arg, curr_scope, func_val_bounds);

                    merge_and_queue_regions(f_queue, regions, arg_regions, prods,
                                            env, only_regions_computed, s.func.name());
                } else if (arg.is_image_param() || arg.is_buffer()) {
                    // If the argument is an image or a buffer, the required
                    // bounds are unknown. Create an infinite region of the
                    // correct dimension and update the region map.
                    Buffer<> buf;
                    if (arg.is_image_param()) {
                        buf = arg.image_param.get_buffer();
                    } else {
                        buf = arg.buffer;
                    }
                    map<string, Box> buf_reg;
                    for (int v = 0; v < buf.dimensions(); v++) {
                        buf_reg[buf.name()].push_back(Interval());
                    }
                    merge_regions(regions, buf_reg);
                }
            }
        }

        // Find the regions required for each value of the current function stage,
        // update the region map, and add them to the queue.
        for (const auto &val : def.values()) {
            // Substitute the parameter estimates into the expression and get
            // the regions required for the expression.
            Expr subs_val = SubstituteVarEstimates().mutate(val);
            map<string, Box> curr_regions = boxes_required(subs_val, curr_scope, func_val_bounds);

            // Arguments to the definition may require regions of functions.
            // For example, update definitions in histograms where the bin is
            // based on the value of a function.
            Box left_reg;
            for (const Expr &arg : def.args()) {
                Expr subs_arg = SubstituteVarEstimates().mutate(arg);
                map<string, Box> arg_regions = boxes_required(subs_arg, curr_scope, func_val_bounds);

                // Merge the regions with the regions found while looking at
                // the values.
                merge_regions(curr_regions, arg_regions);

                Interval arg_bounds = bounds_of_expr_in_scope(arg, curr_scope, func_val_bounds);
                left_reg.push_back(arg_bounds);
            }

            auto iter = curr_regions.find(s.func.name());
            if (iter == curr_regions.end()) {
                curr_regions.emplace(s.func.name(), left_reg);
            } else {
                merge_boxes(iter->second, left_reg);
            }

            // Update the region map, and add 'curr_regions' to the queue.
            merge_and_queue_regions(f_queue, regions, curr_regions, prods, env,
                                    only_regions_computed, s.func.name());
        }
        // Remove processed region from the queue.
        f_queue.pop_front();
    }

    // Simplify the bounds on each region and substitute global pipeline
    // bounds for function regions which lower and upper bounds could not be
    // determined.
    map<string, Box> concrete_regions;

    for (auto &f_reg : regions) {
        simplify_box(f_reg.second);

        Box concrete_box;
        for (size_t i = 0; i < f_reg.second.size(); i++) {
            Expr lower = f_reg.second[i].min;
            Expr upper = f_reg.second[i].max;

            auto iter = env.find(f_reg.first);
            bool in_env = (iter != env.end());

            if (!lower.as<IntImm>() && in_env) {
                const Function &curr_f = iter->second;
                for (const auto &b : curr_f.schedule().estimates()) {
                    size_t num_pure_args = curr_f.args().size();
                    if ((i < num_pure_args) && (b.var == curr_f.args()[i])) {
                        lower = Expr(b.min.as<IntImm>()->value);
                    }
                }
            }

            if (!upper.as<IntImm>() && in_env) {
                const Function &curr_f = iter->second;
                for (const auto &b : curr_f.schedule().estimates()) {
                    size_t num_pure_args = curr_f.args().size();
                    if ((i < num_pure_args) && (b.var == curr_f.args()[i])) {
                        const IntImm *bmin = b.min.as<IntImm>();
                        const IntImm *bextent = b.extent.as<IntImm>();
                        upper = Expr(bmin->value + bextent->value - 1);
                    }
                }
            }

            Interval concrete_bounds = Interval(lower, upper);
            concrete_box.push_back(concrete_bounds);
        }
        concrete_regions[f_reg.first] = concrete_box;
    }
    return concrete_regions;
}

/** Return redundantly computed regions of producers ('prods') while computing a
 * region of the function stage ('f', 'stage_num') specified by 'bounds'. 'var'
 * is the dimension along which redundant computation is accounted for. */
map<string, Box>
DependenceAnalysis::redundant_regions(Function f, int stage_num, string var,
                                      const DimBounds &bounds,
                                      const set<string> &prods,
                                      bool only_regions_computed) {
    // Find the regions required to compute the region of 'f' specified
    // by 'bounds'.
    map<string, Box> regions = regions_required(f, stage_num, bounds,
                                                prods, only_regions_computed);

    // Shift the bounds by the size of the interval along the direction
    // of var.
    DimBounds shifted_bounds;

    for (const auto &b : bounds) {
        if (b.first == var) {
            Expr len = b.second.max - b.second.min + 1;
            Interval bound = Interval(b.second.min + len, b.second.max + len);
            shifted_bounds[b.first] = bound;
        } else {
            shifted_bounds[b.first] = b.second;
        }
    }

    // Find the regions required to compute the region of f specified
    // by shifted_bounds.
    map<string, Box> regions_shifted = regions_required(f, stage_num, shifted_bounds,
                                                        prods, only_regions_computed);

    // Compute the overlaps between 'regions_shifted' and the original
    // regions required.
    map<string, Box> overlaps;
    for (const auto &reg : regions) {
        auto iter = regions_shifted.find(reg.first);
        if (iter == regions.end()) {
            // It will be interesting to log cases where this actually happens
            // i.e., the shifted regions do not contain a function that was
            // there in the original regions.
            continue;
        }
        const Box &b = reg.second;
        const Box &b_shifted = iter->second;
        // The boxes should be of the same size.
        internal_assert(b.size() == b_shifted.size());

        Box b_intersect;
        for (uint32_t i = 0 ; i < b.size(); i++) {
            b_intersect.push_back(Interval::make_intersection(b[i], b_shifted[i]));
        }
        // A function should appear once in the regions and therefore cannot
        // already be present in the overlaps map.
        internal_assert(overlaps.find(reg.first) == overlaps.end());
        overlaps.emplace(reg.first, b_intersect);
    }

    // Simplify the bounds of each of the overlap regions.
    for (auto &f : overlaps) {
        simplify_box(f.second);
    }

    return overlaps;
}

/** Return overlapping regions of producers ('prods') while computing a function
 * stage along each of the dimensions. */
vector<map<string, Box>>
DependenceAnalysis::overlap_regions(Function f, int stage_num,
                                    const DimBounds &bounds,
                                    const set<string> &prods,
                                    bool only_regions_computed) {
    vector<map<string, Box>> conc_overlaps;

    Definition def = get_stage_definition(f, stage_num);
    const vector<Dim> &dims = def.schedule().dims();

    // Get the redundant regions along each dimension of f.
    for (int d = 0; d < (int)dims.size() - 1; d++) { // Ignore '__outermost'
        map<string, Box> conc_reg = redundant_regions(f, stage_num, dims[d].var, bounds,
                                                      prods, only_regions_computed);
        conc_overlaps.push_back(conc_reg);
    }
    return conc_overlaps;
}

/** Return the regions of each function required for computing the
 * outputs of the pipeline. */
map<string, Box> get_pipeline_bounds(DependenceAnalysis &analysis,
                                     const vector<Function> &outputs) {
    map<string, Box> pipeline_bounds;

    // Find the regions required for each of the outputs and merge them
    // to compute the full pipeline_bounds.
    for (const auto &out : outputs) {
        DimBounds pure_bounds;
        Box out_box;
        // Use the estimates on the output for determining the output bounds.
        // If there are duplicates, use the most recent estimate.
        const auto &estimates = out.schedule().estimates();
        for (const auto &arg : out.args()) {
            bool estimate_found = false;
            for (int i = estimates.size() - 1; i >= 0; --i) {
                const auto &est = estimates[i];
                if (est.var == arg) {
                    Interval I = Interval(est.min, simplify(est.min + est.extent - 1));
                    pure_bounds.emplace(arg, I);
                    out_box.push_back(I);
                    estimate_found = true;
                    break;
                }
            }
            if (!estimate_found) {
                pure_bounds.emplace(arg, Interval());
            }
        }

        set<string> prods;
        for (const pair<string, Function> &fpair : analysis.env) {
            prods.insert(fpair.first);
        }

        map<string, Box> regions = analysis.regions_required(out, pure_bounds, prods, false);

        // Add the output region to the pipeline bounds as well.
        regions.emplace(out.name(), out_box);

        merge_regions(pipeline_bounds, regions);
    }

    return pipeline_bounds;
}

/** Implement the grouping algorithm and the cost model for making the grouping
 * choices. */
struct Partitioner {
    /** GroupingChoice encodes the grouping of the 'prod' function into the 'cons' stage. */
    struct GroupingChoice {
        string prod;
        FStage cons;

        GroupingChoice(const string &prod, const FStage &cons) : prod(prod), cons(cons) {}

        bool operator==(const GroupingChoice &other) const {
            return (prod == other.prod) && (cons == other.cons);
        }

        bool operator<(const GroupingChoice &other) const {
            return (prod < other.prod) || ((prod == other.prod) && (cons < other.cons));
        }

        friend std::ostream& operator<<(std::ostream &stream, const GroupingChoice &choice) {
            stream << "Choice: " << choice.prod << " -> " << choice.cons << '\n';
            return stream;
        }
    };

    /** A group is a sub-pipeline with a single output. Members of a group are
     * either inlined into the consumer functions within the group or computed
     * at tiles of the output, specified by 'tile_sizes'.
     *
     * TODO: The restriction of computing either at the inline or tile level
     * makes the space of scheduling choices for a group very tractable.
     * However, the restriction might miss good schedules which can only be
     * realized by computing the members of the group at different levels of
     * the group.
     *
     * There are two approaches to extend the space of schedules considered:
     * 1) Recursive grouping: Treat the problem of determining the compute levels
     * within a group as a smaller instance of the grouping problem with
     * different parameters for the input, output sizes, and cache model.
     *
     * 2) Tightening: Always compute a function at the lowest level possible
     * without introducing redundant work. This is a restricted form of recursive
     * grouping which does not explore the trade-off between redundant work and
     * locality.
     *
     * Either approach can be implemented as a post process for each group
     * after the initial grouping process finishes. The cost model may
     * already make sub-optimal higher level partitioning when it is not aware
     * of the benefits of the post processing. However, it should strictly be
     * an improvement over the initial grouping. As a first step, it is good
     * to make it a post process.
     *
     * Incorporating the recursive grouping process into the cost model can be
     * tricky and can potentially make the cost of analyzing a group
     * prohibitive, as it requires solving smaller instances of the grouping
     * problem for analyzing each configuration. On the other hand, tightening
     * can be integrated into the cost model with out significantly increasing
     * the time to analyze a grouping configuration.
     *
     * TODO: Sliding window schedules can be implemented as a post-pass by
     * moving the store level of all the members of the group to the outermost
     * serial loop. It can be incorporated in the cost model with some effort.
     *
     * TODO: Register tiling is an important transformation especially for
     * benchmarks with significant reuse of the data (like matrix multiply and
     * convolutional layers). The mechanism for realizing register tiling is to
     * completely unroll small tiles of the innermost kernels. Unrolling
     * interacts with vectorization, storage layout, and depends on the outer
     * level tiling. */
    struct Group {
        // The output stage representing the group.
        FStage output;
        // Functions that belong to the group.
        vector<FStage> members;
        // Members of the group which are inlined.
        set<string> inlined;
        // Tile sizes along dimensions of the output function of the group.
        map<string, int> tile_sizes;

        Group(const FStage &output, const vector<FStage> &members)
            : output(output), members(members) {}

        friend std::ostream& operator<<(std::ostream &stream, const Group &g) {
            stream << "Output FStage: " << g.output << '\n';
            stream << "Members: " << '{';
            for (size_t i = 0; i < g.members.size(); ++i) {
                if (i > 0) {
                    stream << ", ";
                }
                stream << g.members[i];
            }
            stream << "}" << '\n';

            stream << "Inlined: " << '{';
            for (auto iter = g.inlined.begin(); iter != g.inlined.end(); ++iter) {
                if (std::distance(g.inlined.begin(), iter) > 0) {
                    stream << ", ";
                }
                stream << *iter;
            }
            stream << "}" << '\n';

            stream << "Tile sizes: " << "{";
            for (auto iter = g.tile_sizes.begin(); iter != g.tile_sizes.end(); ++iter) {
                if (std::distance(g.tile_sizes.begin(), iter) > 0) {
                    stream << ", ";
                }
                stream << "(" << iter->first << ", " <<  iter->second << ")";
            }
            stream << "}" << '\n';

            return stream;
        }
    };

    /** Result of the analysis of a group. */
    struct GroupAnalysis {
        // Estimate of the arithmetic and memory cost for computing the group.
        Cost cost;
        // Estimate of the parallelism that can be exploited while computing
        // the group.
        int64_t parallelism;

        friend std::ostream& operator<<(std::ostream &stream, const GroupAnalysis &analysis) {
            stream << "[arith cost:" << analysis.cost.arith << ", ";
            stream << "memory cost:" << analysis.cost.memory << ", ";
            stream << "parallelism:" << analysis.parallelism << "]\n";
            return stream;
        }
    };

    /** Configuration of a group and the corresponding analysis. A group is the
     * set of functions that are computed together in tiles and the group config
     * specifies at what granularity they are computed together ('tile_sizes'). */
    struct GroupConfig {
        map<string, int> tile_sizes;
        GroupAnalysis analysis;
        GroupConfig(const map<string, int> &tile_sizes, const GroupAnalysis &analysis)
            : tile_sizes(tile_sizes), analysis(analysis) {}
    };

    /** Cache for storing the best configuration for the grouping choice. During
     * the grouping process, the impact of grouping two groups together is only
     * limited to the producers and consumers of the groups that are being grouped
     * together. The best grouping choices for the rest of the pipeline need not be
     * re-evaluated and caching them improves performance significantly. */
    map<GroupingChoice, GroupConfig> grouping_cache;

    /** Each group in the pipeline has a single output stage. A group is comprised
     * of function stages that are computed together in tiles (stages of a function
     * are always grouped together). 'groups' is the mapping from the output stage
     * of the group to the group. */
    map<FStage, Group> groups;
    /** The child stages of each stage (i.e. stages that depend on or use the values
     * computed by a particular stage) in the pipeline. */
    map<FStage, set<FStage>> children;
    /** Map from the output stage of the group to the analysis of the group. The mapping
     * needs to be updated whenever the grouping changes. */
    map<FStage, GroupAnalysis> group_costs;

    /** Levels that are targeted by the grouping algorithm. In the 'INLINE' mode, the grouping
     * algorithm groups the functions by inlining the expression for the producer function
     * into the consumer stage. In the 'FAST_MEM' mode, the grouping is done at the level of
     * tiles of the group output stage. */
    enum Level {INLINE, FAST_MEM};

    /** Bounds of each function stage in the pipeline. These bounds are inferred from the
     * estimates of the outputs and other functions in the pipeline. */
    const map<string, Box> &pipeline_bounds;
    /** Parameters of the machine model that is used for estimating the cost of each
     * group in the pipeline. */
    const MachineParams &arch_params;
    /** Dependency analysis of the pipeline. This support queries on regions
     * accessed and computed for producing some regions of some functions. */
    DependenceAnalysis &dep_analysis;
    /** The arithmetic and memory costs of evaluating the expressions which define
     * each function in the pipeline. */
    RegionCosts &costs;
    /** Output functions of the pipeline. */
    const vector<Function> &outputs;

    Partitioner(const map<string, Box> &_pipeline_bounds, const MachineParams &_arch_params,
                DependenceAnalysis &_dep_analysis, RegionCosts &_costs,
                const vector<Function> &_outputs);

    void merge_groups(const GroupingChoice &choice, const GroupConfig &eval,
                      Partitioner::Level level);

    /** Merge 'prod_group' into 'cons_group'. The output stage of 'cons_group'
     * will be the output stage of the merged group.
     */
    Group merge_groups(const Group &prod_group, const Group &cons_group);

    /** Given a grouping 'g', compute the estimated cost (arithmetic + memory) and
     * parallelism that can be potentially exploited when computing that group.
     */
    GroupAnalysis analyze_group(const Group &g, bool show_analysis);

    map<FStage, map<FStage, DimBounds>> group_loop_bounds();
    map<FStage, map<string, Box>> group_storage_bounds();

    GroupConfig evaluate_choice(const GroupingChoice &group, Partitioner::Level level);

    void group(Partitioner::Level level);

    vector<pair<GroupingChoice, GroupConfig>>
    choose_candidate_grouping(const vector<pair<string, string>> &cand_pairs,
                              Partitioner::Level level);

    map<string, int64_t> evaluate_reuse(const FStage &stg,
                                        const set<string> &prods);

    map<string, int64_t> analyze_spatial_locality(
        const FStage &stg, const map<string, Box> &parent_bounds,
        const set<string> &inlines = set<string>());

    int64_t find_max_access_stride(const Scope<int> &vars, const string &func_acc,
                                   const vector<Expr> &acc_exprs, const Box &buffer_bounds);

    map<string, int64_t> bounds_to_estimates(const DimBounds &bounds);

    string generate_cpu_schedule(const Target &t);

    string generate_group_cpu_schedule(const Group &g, const Target &t,
                                       const map<FStage, DimBounds> &group_loop_bounds,
                                       const map<string, Box> &group_storage_bounds,
                                       const set<string> &inlines);

    /** Return the bounds required to produce a function stage. */
    DimBounds get_bounds(const FStage &stg);

    DimBounds get_bounds_from_tile_sizes(const FStage &stg,
                                         const map<string, int> &tile_sizes);

    /** Given a function stage, return a vector of possible tile configurations for
     * that function stage.
     */
    vector<map<string, int>> generate_tile_configs(const FStage &stg);

    /** Find the best tiling configuration for a group 'g' among a set of tile
     * configurations. This returns a pair of configuration with the highest
     * estimated benefit and the estimated benefit. */
    pair<map<string, int>, GroupAnalysis> find_best_tile_config(const Group &g);

    /** Estimate the benefit (arithmetic + memory) of 'new_grouping' over 'old_grouping'.
     * Positive values indicates that 'new_grouping' may be preferrable over 'old_grouping'.
     * When 'ensure_parallelism' is set to true, this will return an unknown cost
     * if the estimated parallelism is smaller than the machine parameters.
     * If 'no_redundant_work' is set, we only consider the arithmetic cost, i.e. if
     * the arithmetic benefit is negative, we will treat it as no benefits and we
     * should not perform the new grouping.
     */
    int64_t estimate_benefit(const GroupAnalysis &old_grouping, const GroupAnalysis &new_grouping,
                             bool no_redundant_work, bool ensure_parallelism);

    int64_t estimate_benefit(const vector<pair<GroupingChoice, GroupConfig>> &choices,
                             bool no_redundant_work, bool ensure_parallelism);

    void initialize_groups();

    /** Return the total estimate on arithmetic and memory costs of computing all
     * groups within the pipeline. */
    Cost get_pipeline_cost();

    /** Helper function to display partition information of the pipeline. */
    // @{
    void disp_pipeline_costs(int dlevel);
    void disp_pipeline_bounds(int dlevel);
    void disp_pipeline_graph(int dlevel);
    void disp_grouping(int dlevel);
    // @}
};

void Partitioner::disp_grouping(int dlevel = debug_level) {
    debug(dlevel) << "\n=========" << '\n';
    debug(dlevel) << "Grouping:" << '\n';
    debug(dlevel) << "=========" << '\n';
    for (const auto &g : groups) {
        debug(dlevel) << g.second << '\n';
    }
    debug(dlevel) << "=========" << '\n';
}

void Partitioner::disp_pipeline_graph(int dlevel = debug_level) {
    debug(dlevel) << "\n================" << '\n';
    debug(dlevel) << "Pipeline graph:" << '\n';
    debug(dlevel) << "================" << '\n';
    for (const auto &f : children) {
        debug(dlevel) << f.first << ": {";
        for (auto iter = f.second.begin(); iter != f.second.end(); ++iter) {
            if (std::distance(f.second.begin(), iter) > 0) {
                debug(dlevel) << ", ";
            }
            debug(dlevel) << *iter;
        }
        debug(dlevel) << "}" << '\n';
    }
    debug(dlevel) << "================" << '\n';
}

void Partitioner::disp_pipeline_bounds(int dlevel = debug_level) {
    debug(dlevel) << "\n================" << '\n';
    debug(dlevel) << "Pipeline bounds:" << '\n';
    debug(dlevel) << "================" << '\n';
    disp_regions(pipeline_bounds, dlevel);
    debug(dlevel) << "===============" << '\n';
}

Cost Partitioner::get_pipeline_cost() {
    internal_assert(group_costs.size() > 0);

    Cost total_cost(0, 0);
    for (const pair<FStage, Group> &g : groups) {
        GroupAnalysis analysis = get_element(group_costs, g.first);
        total_cost.arith += analysis.cost.arith;
        total_cost.memory += analysis.cost.memory;
    }
    return total_cost;
}

void Partitioner::disp_pipeline_costs(int dlevel = debug_level) {
    internal_assert(group_costs.size() > 0);
    Cost total_cost(0, 0);
    debug(dlevel) << "\n===============" << '\n';
    debug(dlevel) << "Pipeline costs:" << '\n';
    debug(dlevel) << "===============" << '\n';
    debug(dlevel) << "Group: (name) [arith cost, mem cost, parallelism]" << '\n';
    for (const pair<FStage, Group> &g : groups) {
        GroupAnalysis analysis = get_element(group_costs, g.first);
        total_cost.arith += analysis.cost.arith;
        total_cost.memory += analysis.cost.memory;

        debug(dlevel) << "Group: " << g.first << " [";
        debug(dlevel) << analysis.cost.arith << ", " << analysis.cost.memory
                      << ", " << analysis.parallelism << "]\n";
    }
    debug(dlevel) << "Total arithmetic cost: " << total_cost.arith << '\n';
    debug(dlevel) << "Total memory cost: " << total_cost.memory << '\n';
    debug(dlevel) << "===============" << '\n';
}

/** Construct a partitioner and build the pipeline graph on which the grouping
 * algorithm operates. */
Partitioner::Partitioner(const map<string, Box> &_pipeline_bounds,
                         const MachineParams &_arch_params,
                         DependenceAnalysis &_dep_analysis,
                         RegionCosts &_costs,
                         const vector<Function> &_outputs)
        : pipeline_bounds(_pipeline_bounds), arch_params(_arch_params),
          dep_analysis(_dep_analysis), costs(_costs), outputs(_outputs) {
    // Place each stage of a function in its own group. Each stage
    // is a node in the pipeline graph.
    for (const auto &f : dep_analysis.env) {
        int num_stages = f.second.updates().size() + 1;
        for (int s = 0; s < num_stages; s++) {
            FStage stg(f.second, s);
            Group g(stg, {stg});
            groups.insert(make_pair(stg, g));
        }
    }

    // Find the consumers of each function and use it to populate the children map.
    for (const auto &f : dep_analysis.env) {
        int num_stages = f.second.updates().size() + 1;
        for (int s = 0; s < num_stages; s++) {

            set<string> parents = get_parents(f.second, s);

            for (const string &c : parents) {
                // Filter out the calls to pipeline inputs. 'env' only contains
                // the functions computed and not the inputs.
                auto iter = dep_analysis.env.find(c);
                if ((c != f.first) && (iter != dep_analysis.env.end())) {
                    // Consumer depends only on the last stage of a producer
                    // with multiple stages.
                    const Function &prod_func = iter->second;
                    int final_stage = prod_func.updates().size();

                    FStage prod_stage(prod_func, final_stage);
                    FStage cons_stage(f.second, s);

                    children[prod_stage].insert(cons_stage);
                }
            }

            if (s > 0) {
                // Update the children map to reflect the dependencies between
                // different stages of the same function.
                FStage prod_stage(f.second, s - 1);
                FStage cons_stage(f.second, s);

                children[prod_stage].insert(cons_stage);
            }
        }
    }
}

void Partitioner::merge_groups(const GroupingChoice &choice, const GroupConfig &eval,
                               Partitioner::Level level) {
    Function prod_f = get_element(dep_analysis.env, choice.prod);
    size_t num_stages = prod_f.updates().size() + 1;

    const FStage &child = choice.cons;
    Group &child_group = get_element(groups, child);

    for (size_t s = 0; s < num_stages; s++) {
        FStage cand(prod_f, s);

        internal_assert(groups.find(child) != groups.end());
        Group &cand_group = get_element(groups, cand);

        vector<FStage> cand_funcs = cand_group.members;

        vector<FStage> &child_group_members = child_group.members;
        child_group_members.insert(child_group_members.end(),
                                   cand_funcs.begin(), cand_funcs.end());

        if (level == Partitioner::INLINE) {
            for (const auto &stg : cand_funcs) {
                child_group.inlined.insert(stg.func.name());
            }
        } else {
            for (const auto &in : cand_group.inlined) {
                child_group.inlined.insert(in);
            }
        }
    }

    child_group.tile_sizes = eval.tile_sizes;

    // Update group costs.
    // TODO: check if this is necessary or if the analysis from eval can just
    // be reused.
    group_costs[child] = analyze_group(child_group, false);
}

void Partitioner::initialize_groups() {
    for (pair<const FStage, Group> &g : groups) {
        pair<map<string, int>, GroupAnalysis> best = find_best_tile_config(g.second);
        g.second.tile_sizes = best.first;
        group_costs.emplace(g.second.output, best.second);
    }
    grouping_cache.clear();
}

map<string, int64_t> Partitioner::evaluate_reuse(const FStage &stg,
                                                 const set<string> &prods) {
    map<string, int64_t> reuse;
    Function f = stg.func;

    Definition def = get_stage_definition(stg.func, stg.stage_num);

    // TODO: Check if tile sizes of 1 in each dimension gives a reasonable
    // answer or reuse should be evaluated at a much larger granularity or
    // symbolically.  Using a symbolic version might be better if the objective
    // is to prove the dimension has no reuse. The only downside with the
    // symbolic method is it totally at the mercy of the simplifier.  Another
    // option is sampling or using a larger granularity.
    map<string, int> tile_sizes;

    const vector<Dim> &dims = def.schedule().dims();
    for (int d = 0; d < (int)dims.size() - 1; d++) { // Ignore '__outermost'
        tile_sizes[dims[d].var] = 1;
    }

    DimBounds bounds = get_bounds_from_tile_sizes(stg, tile_sizes);

    vector<map<string, Box>> reuse_regions =
        dep_analysis.overlap_regions(stg.func, stg.stage_num, bounds, prods, false);

    for (size_t d = 0; d < dims.size() - 1; d++) { // Ignore '__outermost'
        int64_t total_reuse = 0;
        disp_regions(reuse_regions[d]);
        for (const auto &reg : reuse_regions[d]) {
            int64_t size = box_size(reg.second);
            if (size != unknown) {
                total_reuse += size;
            } else {
                total_reuse = unknown;
                break;
            }
        }
        reuse[dims[d].var] = total_reuse;
    }

    return reuse;
}

/** Pick the best choice among all the grouping options currently available. Uses
 * the cost model to estimate the benefit of each choice. Returns a vector of
 * choice and configuration pairs which describe the best grouping choice. */
vector<pair<Partitioner::GroupingChoice, Partitioner::GroupConfig>>
Partitioner::choose_candidate_grouping(const vector<pair<string, string>> &cands,
                                       Partitioner::Level level) {
    vector<pair<GroupingChoice, GroupConfig>> best_choices;
    int64_t best_benefit = 0;
    for (const auto &p : cands) {
        // Compute the aggregate benefit for inlining into all the children.
        vector<pair<GroupingChoice, GroupConfig>> choices;

        Function prod_f = get_element(dep_analysis.env, p.first);
        int final_stage = prod_f.updates().size();

        FStage prod(prod_f, final_stage);

        for (const FStage &c : children[prod]) {

            GroupAnalysis tmp;
            GroupConfig best_config(map<string, int>(), tmp);
            GroupingChoice cand_choice(prod_f.name(), c);

            // Check if the candidate has been evaluated for grouping before
            if (grouping_cache.find(cand_choice) != grouping_cache.end()) {
                best_config = get_element(grouping_cache, cand_choice);
            } else {
                best_config = evaluate_choice(cand_choice, level);
                // Cache the result of the evaluation for the pair
                grouping_cache.emplace(cand_choice, best_config);
            }

            choices.push_back(make_pair(cand_choice, best_config));
        }

        bool no_redundant_work = false;
        int64_t overall_benefit = estimate_benefit(choices, no_redundant_work, true);

        for (const auto &choice : choices) {
            debug(debug_level) << "Cand choice: " << choice.first;
        }
        debug(debug_level) << "Cand benefit: " << overall_benefit << '\n';
        // TODO: The grouping process can be non-deterministic when the costs
        // of two choices are equal
        if (best_benefit < overall_benefit) {
            best_choices = choices;
            best_benefit = overall_benefit;
        }
    }

    for (const auto &choice : best_choices) {
        debug(debug_level) << "\nBest choice: " << choice.first;
    }
    if (best_choices.size() > 0) {
        debug(debug_level) << "Best benefit: " << best_benefit << '\n';
    }

    return best_choices;
}

vector<map<string, int>> Partitioner::generate_tile_configs(const FStage &stg) {
    // TODO: This is a wart due to the cost model not taking vectorization
    // and pre-fetching into account. Ensuring the innermost dimension has
    // at least size of 64 gives enough values for vectorization and can help
    // with prefetching. This also interacts with the number of parallel tasks
    // that are generated.
    int min_inner_dim_size = 64;

    Definition def = get_stage_definition(stg.func, stg.stage_num);
    const vector<Dim> &dims = def.schedule().dims();

    // Get the dimensions that are going to be tiled in this stage.
    // Skipping rvars for now.
    vector<string> tile_vars;
    for (int d = 0; d < (int)dims.size() - 1; d++) { // Ignore '__outermost'
        if (!dims[d].is_rvar()) {
            tile_vars.push_back(dims[d].var);
        }
    }

    vector<int> size_variants = {1, 4, 8, 16, 32, 64, 128, 256};
    vector<map<string, int>> tile_configs;

    // For all the tile configurations generated, we force the innermost dimension
    // to be at least of size 64 to ensure enough values for vectorization.

    // TODO: Add comments explaining the different tiling schemes.

    // Skewed tile configurations
    for (size_t i = 0; i < tile_vars.size(); i++) {
        for (const auto &dim_size : size_variants) {
            map<string, int> tiling;
            tiling[tile_vars[i]] =
                (i == 0) ? std::max(dim_size, min_inner_dim_size): dim_size;
            for (size_t j = 0; j < tile_vars.size(); j++) {
                if (j < i) {
                    tiling[tile_vars[j]] = size_variants[size_variants.size() - 1];
                } else if (j > i) {
                    tiling[tile_vars[j]] = size_variants[0];
                }
            }
            if (!tiling.empty()) {
                bool is_duplicate =
                    std::find_if(tile_configs.begin(), tile_configs.end(),
                                [&tiling](const map<string, int> &m) { return (tiling == m);})
                    != tile_configs.end();
                if (!is_duplicate) {
                    tile_configs.push_back(tiling);
                }
            }
        }
    }

    // Almost square tile configurations
    for (const auto &dim_size : size_variants) {
        map<string, int> tiling;
        for (size_t j = 0; j < tile_vars.size(); j++) {
            tiling[tile_vars[j]] = j == 0 ?
                            std::max(dim_size, min_inner_dim_size): dim_size;
        }
        if (!tiling.empty()) {
            bool is_duplicate =
                std::find_if(tile_configs.begin(), tile_configs.end(),
                            [&tiling](const map<string, int> &m) { return (tiling == m);})
                != tile_configs.end();
            if (!is_duplicate) {
                tile_configs.push_back(tiling);
            }
        }
    }

    // Reorder tile configurations
    for (int i = 0; i < (1 << (tile_vars.size())); i++) {
        map<string, int> tiling;
        for (size_t j = 0; j < tile_vars.size(); j++) {
            if (((i >> (j)) & 1) == 1) {
                if (j == 0) {
                    tiling[tile_vars[j]] = min_inner_dim_size;
                } else {
                    tiling[tile_vars[j]] = 1;
                }
            }
        }
        if (!tiling.empty()) {
            bool is_duplicate =
                std::find_if(tile_configs.begin(), tile_configs.end(),
                            [&tiling](const map<string, int> &m) { return (tiling == m);})
                != tile_configs.end();
            if (!is_duplicate) {
                tile_configs.push_back(tiling);
            }
        }
    }

    return tile_configs;
}

pair<map<string, int>, Partitioner::GroupAnalysis>
Partitioner::find_best_tile_config(const Group &g) {
    // Initialize to no tiling
    map<string, int> no_tile_config;
    Group no_tile = g;
    no_tile.tile_sizes = no_tile_config;

    bool show_analysis = false;
    GroupAnalysis no_tile_analysis = analyze_group(no_tile, show_analysis);

    GroupAnalysis best_analysis = no_tile_analysis;
    map<string, int> best_config = no_tile_config;
    if (best_analysis.cost.arith == unknown) {
        return make_pair(best_config, best_analysis);
    }

    // Generate tiling configurations
    vector<map<string, int>> configs = generate_tile_configs(g.output);

    Group best_group = g;
    for (const auto &config : configs) {
        Group new_group = g;
        new_group.tile_sizes = config;

        GroupAnalysis new_analysis = analyze_group(new_group, show_analysis);

        bool no_redundant_work = false;
        int64_t benefit = estimate_benefit(best_analysis, new_analysis,
                                           no_redundant_work, true);

        if (show_analysis) {
            debug(debug_level) << "Benefit relative to not tiling:" << benefit << '\n';
            debug(debug_level) << "Best analysis:" << new_analysis;
            debug(debug_level) << "No tile analysis:" << no_tile_analysis;
            debug(debug_level)
                << "arith cost:" << (float)new_analysis.cost.arith / no_tile_analysis.cost.arith
                << ", mem cost:" << (float)new_analysis.cost.memory / no_tile_analysis.cost.memory << '\n';
        }

        if (benefit > 0) {
            best_config = config;
            best_analysis = new_analysis;
            best_group = new_group;
        }
    }

    debug(debug_level) << "\nBest grouping:\n" << best_group << '\n';

    return make_pair(best_config, best_analysis);
}

/** Partition the pipeline by iteratively merging groups until a fixpoint is
 * reached. */
void Partitioner::group(Partitioner::Level level) {
    bool fixpoint = false;
    while (!fixpoint) {
        Cost pre_merge = get_pipeline_cost();

        fixpoint = true;
        vector<pair<string, string>> cand;
        for (const pair<FStage, Group> &g : groups) {
            bool is_output = false;
            for (const Function &f : outputs) {
                if (g.first.func.name() == f.name()) {
                    is_output = true;
                    break;
                }
            }

            // All stages of a function are computed at a single location.
            // The last stage of the function represents the candidate choice
            // of grouping the function into a consumer.

            const Function &prod_f = get_element(dep_analysis.env, g.first.func.name());
            bool is_final_stage = (g.first.stage_num == prod_f.updates().size());

            if (is_output || !is_final_stage) {
                continue;
            }

            const auto &iter = children.find(g.first);
            if (iter != children.end()) {
                // All the stages belonging to a function are considered to be a
                // single child.
                set<string> child_groups;
                for (const FStage &s : iter->second) {
                    child_groups.insert(s.func.name());
                }

                int num_children = child_groups.size();
                // Only groups with a single child are considered for grouping
                // when grouping for computing in tiles. The scheduling model
                // does not allow functions to be computed at different points.
                if ((num_children == 1) && (level == Partitioner::FAST_MEM)) {
                    const string &prod_name = prod_f.name();
                    const string &cons_name = (*child_groups.begin());
                    cand.push_back(make_pair(prod_name, cons_name));
                } else if((level == Partitioner::INLINE) && prod_f.is_pure()) {
                    const string &prod_name = prod_f.name();
                    cand.push_back(make_pair(prod_name, ""));
                }
            }
        }

        debug(debug_level) << "\n============================" << '\n';
        debug(debug_level) << "Current grouping candidates:" << '\n';
        debug(debug_level) << "============================" << '\n';
        for (size_t i = 0; i < cand.size(); ++i) {
            if (i > 0) {
                debug(debug_level) << ", ";
            }
            debug(debug_level) << "{" << cand[i].first << ", " << cand[i].second << "}" << '\n';
        }

        vector<pair<GroupingChoice, GroupConfig>> best = choose_candidate_grouping(cand, level);
        if (best.empty()) {
            continue;
        } else {
            fixpoint = false;
        }

        // The following code makes the assumption that all the stages of a function
        // will be in the same group. 'choose_candidate_grouping' ensures that the
        // grouping choice being returned adheres to this constraint.
        const string &prod = best[0].first.prod;

        Function prod_f = get_element(dep_analysis.env, prod);
        size_t num_stages = prod_f.updates().size() + 1;

        FStage final_stage(prod_f, num_stages - 1);
        set<FStage> prod_group_children = get_element(children, final_stage);

        // Invalidate entries of the grouping cache
        set<GroupingChoice> invalid_keys;
        for (const auto &c : prod_group_children) {
            for (const auto &entry : grouping_cache) {
                if ((entry.first.prod == c.func.name()) || (entry.first.cons == c)) {
                    invalid_keys.insert(entry.first);
                }
            }
        }
        for (const auto &key : invalid_keys) {
            grouping_cache.erase(key);
        }

        for (const auto &group : best) {
            internal_assert(group.first.prod == prod);
            merge_groups(group.first, group.second, level);
        }

        for (size_t s = 0; s < num_stages; s++) {
            FStage prod_group(prod_f, s);
            groups.erase(prod_group);
            group_costs.erase(prod_group);

            // Update the children mapping
            children.erase(prod_group);
            for (auto &f : children) {
                set<FStage> &cons = f.second;
                auto iter = cons.find(prod_group);
                if (iter != cons.end()) {
                    cons.erase(iter);
                    // For a function with multiple stages all the stages will
                    // be in the same group and the consumers of the function
                    // only depend on the last stage. Therefore, when the
                    // producer group has multiple stages, parents of the
                    // producers should point to the consumers of the last
                    // stage of the producer.
                    cons.insert(prod_group_children.begin(), prod_group_children.end());
                }
            }
        }

        Cost post_merge = get_pipeline_cost();

        disp_pipeline_costs();

        internal_assert((pre_merge.arith + pre_merge.memory) >=
                        (post_merge.arith + post_merge.memory));
    }
}

DimBounds Partitioner::get_bounds(const FStage &s) {
    Definition def = get_stage_definition(s.func, s.stage_num);
    DimBounds bounds;

    const vector<string> &args = s.func.args();
    for (size_t d = 0; d < args.size(); d++) {
        bounds[args[d]] = get_element(pipeline_bounds, s.func.name())[d];
    }

    return get_stage_bounds(s.func, s.stage_num, bounds);
}

DimBounds Partitioner::get_bounds_from_tile_sizes(const FStage &s,
                                                  const map<string, int> &tile_sizes) {
    Definition def = get_stage_definition(s.func, s.stage_num);
    map<string, Interval> bounds;

    const map<string, Interval> &def_bounds = get_bounds(s);
    const vector<Dim> &dims = def.schedule().dims();

    for (int d = 0; d < (int)dims.size() - 1; d++) { // Ignore '__outermost'
        string var = dims[d].var;
        const Interval &bound = get_element(def_bounds, var);
        const auto &iter = tile_sizes.find(var);
        if (iter != tile_sizes.end()) {
            int size = iter->second;
            // Check if the bounds allow for tiling with the given tile size,
            // i.e. ensure at least 2 tiles
            int64_t extent = get_extent(bound);
            if (extent >= 2 * size) {
                // TODO: Maybe shift this to the center of the pipeline bound
                bounds[var] = Interval(0, size - 1);
            } else {
                // If the dimension is too small, do not tile it and set the
                // extent of the bounds to that of the dimension estimate
                bounds[var] = bound;
            }
        } else {
            bounds[var] = bound;
        }
    }

    return bounds;
}

Partitioner::GroupAnalysis Partitioner::analyze_group(const Group &g, bool show_analysis) {
    // Get the definition corresponding to the group output
    Definition def = get_stage_definition(g.output.func, g.output.stage_num);

    set<string> group_inputs;
    set<string> group_members;

    for (const auto &stg : g.members) {
        group_members.insert(stg.func.name());
        set<string> parents = get_parents(stg.func, stg.stage_num);
        for (const auto &c : parents) {
            bool is_member = false;
            for (const auto &m : g.members) {
                if (m.func.name() == c) {
                    is_member = true;
                    break;
                }
            }
            if (!is_member) {
                group_inputs.insert(c);
            }
        }
    }

    // Count the number of tiles
    uint64_t estimate_tiles = 1;
    uint64_t parallelism = 1;
    uint64_t num_ele_per_tile = 1;

    const vector<Dim> &dims = def.schedule().dims();

    DimBounds stg_bounds = get_bounds(g.output);

    GroupAnalysis g_analysis;
    g_analysis.cost = Cost(unknown, unknown);
    g_analysis.parallelism = unknown;

    for (int d = 0; d < (int)dims.size() - 1; d++) { // Ignore '__outermost'
        const string &var = dims[d].var;
        const auto &iter = g.tile_sizes.find(var);
        if (iter != g.tile_sizes.end()) {
            int size = iter->second;
            int64_t extent = get_extent(get_element(stg_bounds, var));
            if (extent == unknown) {
                return g_analysis;
            }

            uint64_t dim_tiles = std::ceil((float)extent / size);
            estimate_tiles *= dim_tiles;
            num_ele_per_tile *= size;
            // Since all Vars are inherently parallelizable by construct, we
            // only need to take RVars into account for the analysis.
            if (can_parallelize_rvar(var, g.output.func.name(), def)) {
                parallelism *= dim_tiles;
            }
        }
    }

    // Get the regions of the pipeline required to compute a tile of the group
    DimBounds tile_bounds = get_bounds_from_tile_sizes(g.output, g.tile_sizes);

    map<string, Box> alloc_regions = dep_analysis.regions_required(
        g.output.func, g.output.stage_num, tile_bounds, group_members, false);

    map<string, Box> compute_regions = dep_analysis.regions_required(
        g.output.func, g.output.stage_num, tile_bounds, group_members, true);

    map<string, Box> group_reg, prod_reg, input_reg;

    // Separating into regions that computed within the group and regions that
    // are input to the group
    for (const auto &reg : compute_regions) {
        if ((group_members.find(reg.first) != group_members.end()) &&
            (reg.first != g.output.func.name())) {
            group_reg.emplace(reg.first, reg.second);
        } else if (group_inputs.find(reg.first) != group_inputs.end()) {
            if (dep_analysis.env.find(reg.first) != dep_analysis.env.end()) {
                prod_reg.emplace(reg.first, reg.second);
            } else {
                input_reg.emplace(reg.first, reg.second);
            }
        }
    }

    // TODO: remove debug code.
    if (show_analysis) {
        debug(0) << "==============\n";
        debug(0) << "Group Analysis\n";
        debug(0) << "==============\n";
        debug(0) << g;
        debug(0) << "\nProd reg:" << '\n';
        disp_regions(prod_reg, 0);
        debug(0) << "Input reg:" << '\n';
        disp_regions(input_reg, 0);
        debug(0) << "Group reg:" << '\n';
        disp_regions(group_reg);
    }

    // Aggregate costs for intermediate functions in a tile and the
    // tile output
    Cost tile_cost = costs.region_cost(group_reg, g.inlined);
    if ((tile_cost.arith == unknown) || (tile_cost.memory == unknown)) {
        return g_analysis;
    }

    Cost out_cost = costs.stage_region_cost(g.output.func.name(),
                                            g.output.stage_num,
                                            tile_bounds, g.inlined);
    if ((out_cost.arith == unknown) || (out_cost.memory == unknown)) {
        return g_analysis;
    }

    for (const auto &reg : alloc_regions) {
        if (box_size(reg.second) == unknown) {
            return g_analysis;
        }
    }

    Cost group_cost(tile_cost.arith + out_cost.arith,
                    tile_cost.memory + out_cost.memory);

    // Detailed load costs for all the group intermediates
    map<string, int64_t> group_load_costs =
        costs.detailed_load_costs(group_reg, g.inlined);

    map<string, int64_t> out_load_costs =
        costs.stage_detailed_load_costs(g.output.func.name(),
                                        g.output.stage_num,
                                        tile_bounds, g.inlined);

    combine_load_costs(group_load_costs, out_load_costs);

    Box out_tile_extent;
    if (g.output.stage_num == 0) {
        const vector<string> &args = g.output.func.args();
        for (size_t d = 0; d < args.size() - 1; d++ ) { // Ignore '__outermost'
            const auto &iter = tile_bounds.find(args[d]);
            if (iter != tile_bounds.end()) {
                out_tile_extent.push_back(iter->second);
            } else {
                out_tile_extent.push_back(Interval());
            }
        }
    }

    Cost per_tile_cost(group_cost.arith, 0);

    // TODO: Add comments on the cost model
    // This is the old cost model; keeping it here for reference, for now.
    /*
    if (tile_inter_size > arch_params.l1_size) {
        // Conservative estimate of accesses to memory
        //per_tile_mem_cost = tile_inter_size;
        // Aggressive estimate of accesses to memory
        per_tile_mem_cost = tile_cost.second;
    } else {
        // The tile_input_size captures the region of the input
        // required to compute the tile. However, all of it many not be
        // accessed during the computation of the tile when the access
        // is sparse. A better estimate is given by the smaller of
        // the number of memory accesses and the region size
        per_tile_mem_cost = std::min(tile_input_size + tile_output_size,
                                     tile_cost.second);
    }*/

    // TODO: Use smooth step curve from Jon to better model cache behavior,
    // where each step corresponds to different cache level.
    //
    // The current cost model drops off linearly. Larger memory footprint is
    // penalized more than smaller memory footprint (since smaller one can fit
    // more in the cache). The cost is clamped at 'balance', which is roughly at
    // memory footprint equal to or larger than the last level cache size.

    // If 'model_reuse' is set, the cost model should take into account memory
    // reuse within the tile, e.g. matrix multiply reuses inputs multiple times.
    // TODO: Implement a better reuse model.
    bool model_reuse = false;

    // Linear dropoff
    float load_slope = (float)arch_params.balance / arch_params.last_level_cache_size;
    for (const auto &f_load : group_load_costs) {
        internal_assert(g.inlined.find(f_load.first) == g.inlined.end())
            << "Intermediates of inlined pure fuction \"" << f_load.first
            << "\" should not have been in the group_load_costs\n";

        const auto &alloc_reg = get_element(alloc_regions, f_load.first);

        int64_t footprint = 0;
        bool is_group_member = (group_members.find(f_load.first) != group_members.end());
        bool is_output = (f_load.first == g.output.func.name());

        // We use allocated region as conservative estimate of the footprint since
        // the loads could be from any random locations of the allocated regions.

        if (!is_output && is_group_member) {
            footprint = costs.region_size(f_load.first, alloc_reg);
        } else {
            int64_t initial_footprint = 0;
            const auto &f_load_pipeline_bounds = get_element(pipeline_bounds, f_load.first);

            bool is_function = (dep_analysis.env.find(f_load.first) != dep_analysis.env.end());
            if (!is_function) { // It is a load to some input buffer
                // Initial loads
                initial_footprint = costs.input_region_size(f_load.first, f_load_pipeline_bounds);
                // Subsequent loads
                footprint = costs.input_region_size(f_load.first, alloc_reg);
            } else if (is_output) { // Load to the output function of the group
                internal_assert(is_group_member)
                    << "Output " << f_load.first << " should have been a group member\n";
                // Initial loads
                initial_footprint = costs.region_size(f_load.first, f_load_pipeline_bounds);
                // Subsequent loads
                footprint = costs.region_size(f_load.first, out_tile_extent);
            } else { // Load to some non-member function (i.e. function from other groups)
                // Initial loads
                initial_footprint = costs.region_size(f_load.first, f_load_pipeline_bounds);
                // Subsequent loads
                footprint = costs.region_size(f_load.first, alloc_reg);
            }

            if (model_reuse) {
                int64_t initial_factor =
                    std::trunc(std::min(1 + initial_footprint * load_slope,
                               (float)arch_params.balance));
                per_tile_cost.memory += initial_factor * footprint;
            } else {
                footprint = initial_footprint;
            }

            if (footprint == unknown) {
                return g_analysis;
            }
        }

        int cost_factor = std::trunc(std::min(1 + footprint * load_slope,
                                     (float)arch_params.balance));
        per_tile_cost.memory += cost_factor * f_load.second;
    }

    if (show_analysis) {
        debug(debug_level) << "\nDetailed loads:\n";
        for (const auto &f_load : group_load_costs) {
            debug(debug_level) << "(" << f_load.first << "," << f_load.second << ")";
        }
        debug(debug_level) << '\n';

        debug(debug_level) << "\nPer tile memory cost:" << per_tile_cost.memory << '\n';
        debug(debug_level) << "Per tile arith cost:" << per_tile_cost.arith << '\n';
    }

    g_analysis.cost.memory = per_tile_cost.memory * estimate_tiles;
    g_analysis.cost.arith = per_tile_cost.arith * estimate_tiles;
    g_analysis.parallelism = parallelism;

    internal_assert(per_tile_cost.memory > 0);

    return g_analysis;
}

Partitioner::Group Partitioner::merge_groups(const Group &prod_group,
                                             const Group &cons_group) {
    vector<FStage> group_members;
    for (const auto &s : prod_group.members) {
        group_members.push_back(s);
    }
    for (const auto &s : cons_group.members) {
        group_members.push_back(s);
    }

    Group group(cons_group.output, group_members);

    for (const auto &f : prod_group.inlined) {
        group.inlined.insert(f);
    }
    for (const auto &f : cons_group.inlined) {
        group.inlined.insert(f);
    }

    return group;
}

Partitioner::GroupConfig Partitioner::evaluate_choice(const GroupingChoice &choice,
                                                      Partitioner::Level level) {
    // Create a group that reflects the grouping choice and evaluate the cost
    // of the group.
    Function prod_f = get_element(dep_analysis.env, choice.prod);
    int num_prod_stages = prod_f.updates().size() + 1;
    vector<Group> prod_groups;

    for (int s = 0; s < num_prod_stages; s++) {
        FStage prod_s(prod_f, s);
        prod_groups.push_back(get_element(groups, prod_s));
    }

    Group cons = get_element(groups, choice.cons);
    Group group = cons;
    for (const auto &prod_g : prod_groups) {
        group = merge_groups(prod_g, group);
    }

    GroupAnalysis group_analysis;
    map<string, int> best_tile_config;

    if (level == Partitioner::INLINE) {
        // Set the tile sizes to one along all dimensions of the consumer group
        map<string, int> tile_sizes;

        const Function &cons_f = cons.output.func;
        Definition def = get_stage_definition(cons_f, cons.output.stage_num);

        const vector<Dim> &dims = def.schedule().dims();
        for (int d = 0; d < (int)dims.size() - 1; d++) {
            tile_sizes[dims[d].var] = 1;
        }

        group.tile_sizes = tile_sizes;

        for (const auto &prod_g : prod_groups) {
            for (const FStage &s : prod_g.members) {
                group.inlined.insert(s.func.name());
            }
        }

        for (const string &f : cons.inlined) {
            group.inlined.insert(f);
        }

        group_analysis = analyze_group(group, false);
        best_tile_config = tile_sizes;

    } else {
        pair<map<string, int>, GroupAnalysis> config = find_best_tile_config(group);
        best_tile_config = config.first;
        group_analysis = config.second;
    }

    return GroupConfig(best_tile_config, group_analysis);
}

int64_t Partitioner::estimate_benefit(const GroupAnalysis &old_grouping,
                                      const GroupAnalysis &new_grouping,
                                      bool no_redundant_work,
                                      bool ensure_parallelism) {
    // TODO: Instead of having a hard parallelism constraint, it may be better
    // to consider other metric, such as arith_cost/parallelism
    if (ensure_parallelism && (new_grouping.parallelism < arch_params.parallelism)) {
        return unknown;
    }

    int64_t arith_benefit = 0;
    if ((old_grouping.cost.arith != unknown) && (new_grouping.cost.arith != unknown)) {
        arith_benefit = old_grouping.cost.arith - new_grouping.cost.arith;
    } else {
        return unknown;
    }

    if (no_redundant_work && arith_benefit < 0) {
        return unknown;
    }

    int64_t mem_benefit = 0;
    if ((old_grouping.cost.memory != unknown) && (new_grouping.cost.memory != unknown)) {
        mem_benefit = old_grouping.cost.memory - new_grouping.cost.memory;
    } else {
        return unknown;
    }

    return mem_benefit + arith_benefit;
}

int64_t Partitioner::estimate_benefit(
        const vector<pair<GroupingChoice, GroupConfig>> &choices,
        bool no_redundant_work, bool ensure_parallelism) {
    GroupAnalysis new_group_analysis;
    new_group_analysis.cost = Cost(0, 0);
    new_group_analysis.parallelism = std::numeric_limits<int64_t>::max();

    set<FStage> no_merge_groups;

    for (const auto &choice : choices) {
        Function prod_f = get_element(dep_analysis.env, choice.first.prod);
        int num_prod_stages = prod_f.updates().size() + 1;
        for (int s = 0; s < num_prod_stages; s++) {
            FStage prod_s(prod_f, s);
            no_merge_groups.insert(prod_s);
        }

        no_merge_groups.insert(choice.first.cons);

        GroupAnalysis analysisg = choice.second.analysis;
        if (analysisg.cost.arith != unknown) {
            new_group_analysis.cost.arith += analysisg.cost.arith;
            new_group_analysis.cost.memory += analysisg.cost.memory;
            new_group_analysis.parallelism = std::min(new_group_analysis.parallelism,
                                                      analysisg.parallelism);
        } else {
            new_group_analysis.cost = Cost(unknown, unknown);
            new_group_analysis.parallelism = unknown;
            break;
        }
    }

    GroupAnalysis old_group_analysis;
    old_group_analysis.cost.arith = 0;
    old_group_analysis.cost.memory = 0;
    old_group_analysis.parallelism = std::numeric_limits<int64_t>::max();

    for (const auto &g : no_merge_groups) {
        const auto &iter = group_costs.find(g);
        internal_assert(iter != group_costs.end());
        GroupAnalysis analysisg = iter->second;
        if (analysisg.cost.arith != unknown) {
            old_group_analysis.cost.arith += analysisg.cost.arith;
            old_group_analysis.cost.memory += analysisg.cost.memory;
            old_group_analysis.parallelism = std::min(old_group_analysis.parallelism,
                                                      analysisg.parallelism);
        } else {
            old_group_analysis.cost = Cost(unknown, unknown);
            old_group_analysis.parallelism = unknown;
            break;
        }
    }

    return estimate_benefit(old_group_analysis, new_group_analysis,
                            no_redundant_work, ensure_parallelism);
}

map<string, int64_t> Partitioner::bounds_to_estimates(const DimBounds &bounds) {
    map<string, int64_t> estimates;
    for (const auto &bound : bounds) {
        int64_t estimate = get_extent(bound.second);
        estimates[bound.first] = estimate;
    }
    return estimates;
}

map<FStage, map<string, Box>> Partitioner::group_storage_bounds() {
    map<FStage, map<string, Box>> group_storage_bounds;
    for (const pair<const FStage, Group> &gpair : groups) {
        Group g = gpair.second;
        DimBounds bounds = get_bounds_from_tile_sizes(g.output, g.tile_sizes);

        set<string> prods;
        for (const FStage &s : g.members) {
            prods.insert(s.func.name());
        }

        map<string, Box> reg_alloc =
            dep_analysis.regions_required(g.output.func, g.output.stage_num,
                                          bounds, prods, false);
        map<string, Box> group_alloc;
        for (const FStage &s : g.members) {
            if (reg_alloc.find(s.func.name()) != reg_alloc.end()
                && s.func.name() != g.output.func.name()) {
                group_alloc[s.func.name()] = reg_alloc[s.func.name()];
            }
        }

        group_storage_bounds[gpair.first] = group_alloc;
    }

    return group_storage_bounds;
}

map<FStage, map<FStage, DimBounds>> Partitioner::group_loop_bounds() {
    map<FStage, map<FStage, DimBounds>> group_bounds;
    for (const pair<const FStage, Group> &gpair : groups) {
        Group g = gpair.second;
        map<FStage, DimBounds> mem_bounds;

        DimBounds bounds = get_bounds_from_tile_sizes(g.output, g.tile_sizes);

        set<string> prods;
        for (const FStage &s : g.members) {
            prods.insert(s.func.name());
        }

        map<string, Box> reg_computed =
            dep_analysis.regions_required(g.output.func, g.output.stage_num,
                                          bounds, prods, true);

        for (const FStage &s : g.members) {
            if (reg_computed.find(s.func.name()) != reg_computed.end()) {
                map<string, int> tile_sizes;
                const vector<string> &args = s.func.args();
                for (size_t arg = 0; arg < args.size(); arg++) {
                    tile_sizes[args[arg]] = get_extent(reg_computed[s.func.name()][arg]);
                }
                mem_bounds[s] = get_bounds_from_tile_sizes(s, tile_sizes);
            }
        }

        group_bounds[gpair.first] = mem_bounds;
    }

    return group_bounds;
}

string get_base_name(string name) {
    size_t dot_pos = name.rfind('.');
    if (dot_pos != string::npos) {
        return name.substr(dot_pos + 1);
    }
    return name;
}

pair<VarOrRVar, VarOrRVar> split_dim(Stage f_handle, VarOrRVar v, int factor,
                                     string in_suffix, string out_suffix,
                                     map<string, int64_t> &estimates, string &sched) {
    // Create new variables for the split dimensions
    string arg_name = v.name();
    string inner_name = arg_name + in_suffix;
    string outer_name = arg_name + out_suffix;
    VarOrRVar inner(inner_name), outer(outer_name);

    sched += "Var " + inner_name + "(\"" + outer_name + "\")" + ";\n";
    sched += "Var " + outer_name + "(\"" + outer_name + "\")" + ";\n";

    f_handle.split(v, outer, inner, factor);

    sched += f_handle.name() + ".split(" + arg_name + ',' +
             outer_name + ',' + inner_name + ',' + std::to_string(factor) + ");\n";

    internal_assert(estimates.find(arg_name) != estimates.end() &&
                    estimates[arg_name] != unknown);

    estimates[inner_name] = factor;
    estimates[outer_name] = std::ceil((float)get_element(estimates, arg_name) / factor);
    estimates.erase(arg_name);

    return make_pair(inner, outer);
}

void vectorize_stage(Stage f_handle, Definition def, Function func,
                     const Target &t, set<string> &rvars,
                     map<string, int64_t> &estimates, string &sched) {
    const vector<Dim> &dims = f_handle.get_schedule().dims();
    int vec_dim_index = -1;

    // Set the vector length as the maximum of the natural vector size of all
    // the values produced by the function.
    int vec_len = 0;
    for (const auto &type : func.output_types()) {
        vec_len = std::max(vec_len, t.natural_vector_size(type));
    }

    for (int d = 0; d < (int) dims.size() - 1; d++) {
        string dim_name = get_base_name(dims[d].var);
        bool can_vectorize = true;
        if (rvars.find(dim_name) != rvars.end()) {
            can_vectorize = can_parallelize_rvar(dim_name, func.name(), def);
        }
        if (estimates.find(dim_name) != estimates.end() &&
            estimates[dim_name] != unknown) {
            if (can_vectorize && estimates[dim_name] >= vec_len) {
                vec_dim_index = d;
                break;
            }
        }
    }

    if (vec_dim_index >= 0) {
        string vec_dim_name = get_base_name(dims[vec_dim_index].var);
        Var vec_var(vec_dim_name);

        bool is_rvar = (rvars.find(vec_dim_name) != rvars.end());

        pair<VarOrRVar, VarOrRVar> split_vars =
            split_dim(f_handle, vec_var, vec_len, "_vi", "_vo", estimates, sched);

        f_handle.vectorize(split_vars.first);
        sched += f_handle.name() + ".vectorize(" + split_vars.first.name() + ");\n";

        if (is_rvar) {
            rvars.erase(vec_dim_name);
            rvars.insert(split_vars.first.name());
            rvars.insert(split_vars.second.name());
        }

        // TODO: Reorder vector dim to the inner most if it is the inner
        // most storage dimension of the func.
        //
        // TODO: Check if the warning is necessary.
        if (vec_dim_index > 0) {
            user_warning << "Outer dim vectorization of var " << vec_dim_name
                         << " in function " << f_handle.name() << '\n';
        }
    }
}

/** Reorder the dimensions to preserve spatial locality. This function
 * checks the stride of the access for each access. The dimensions of
 * the loop are reordered such that the dimension with the smallest
 * access strides is innermost. This takes the strides along each dimension
 * as input. */
void reorder_dims(Stage f_handle, Definition def,
                  map<string, int64_t> strides, string &sched) {
    vector<Dim> &dims = def.schedule().dims();
    vector<pair<string, bool>> order;

    for (int d = 0; d < (int)dims.size() - 1; d++) {
        internal_assert(strides.find(dims[d].var) != strides.end());
    }

    // Iterate until all the dimensions have been assigned an order
    while (strides.size() > 0) {
        // Find the pure dimension with smallest stride
        int64_t min_pure_stride = std::numeric_limits<int64_t>::max();
        string min_pure_var;
        for (int d = 0; d < (int)dims.size() - 1; d++) {
            string var_name = get_base_name(dims[d].var);
            if (strides.find(var_name) != strides.end() &&
                dims[d].is_pure()) {
                int64_t dim_stride = strides[var_name];
                if (dim_stride < min_pure_stride) {
                    min_pure_stride = dim_stride;
                    min_pure_var = var_name;
                }
            }
        }

        // Check if the stride of the pure dimension is smaller than
        // the first reduction dimension that has not been assigned
        // an order yet.
        int64_t min_impure_stride = std::numeric_limits<int64_t>::max();
        string min_impure_var;
        for (int d = 0; d < (int)dims.size() - 1; d++) {
            string var_name = get_base_name(dims[d].var);
            if (strides.find(var_name) != strides.end() &&
                !dims[d].is_pure()) {
                int64_t dim_stride = strides[var_name];
                if (dim_stride < min_impure_stride) {
                    min_impure_stride = dim_stride;
                    min_impure_var = var_name;
                }
                // Reduction dimensions cannot be reordered relative to
                // each other. Stop after encountering the first reduction
                // dimension.
                break;
            }
        }

        pair<string, bool> curr_min_var;
        if (min_impure_stride < min_pure_stride) {
            curr_min_var.first = min_impure_var;
            curr_min_var.second = false;
        } else {
            curr_min_var.first = min_pure_var;
            curr_min_var.second = true;
        }

        order.push_back(curr_min_var);
        strides.erase(curr_min_var.first);
    }

    // TODO: Remove debug code.
    /*
    debug(0) << "Var order for stage:" << f_handle.name() << '\n';
    for (const auto &o : order) {
        debug(0) << o.first << ',';
    }
    debug(0) << '\n';
    */

    vector<VarOrRVar> ordering;
    for (const auto &o : order) {
        VarOrRVar o_var(o.first, o.second);
        ordering.push_back(o_var);
    }

    string var_order = ordering[0].name();
    for (size_t o = 1; o < ordering.size(); o++) {
        var_order += ',' + ordering[o].name();
    }

    f_handle.reorder(ordering);
    sched += f_handle.name() + ".reorder(" + var_order + ");\n";
}

string Partitioner::generate_group_cpu_schedule(
        const Group &g, const Target &t,
        const map<FStage, DimBounds> &group_loop_bounds,
        const map<string, Box> &group_storage_bounds,
        const set<string> &inlines) {
    string sched = "";
    string out_f_name = g.output.func.name();
    Function g_out = g.output.func;

    debug(debug_level) << "\n================\n";
    debug(debug_level) << "Scheduling group:\n";
    debug(debug_level) << "================\n";
    debug(debug_level) << g;

    // Get the definition corresponding to the stage
    Definition def = get_stage_definition(g_out, g.output.stage_num);

    // Get the estimates for stage bounds
    DimBounds stg_bounds = get_bounds(g.output);
    map<string, int64_t> stg_estimates = bounds_to_estimates(stg_bounds);

    Stage f_handle = Stage(Func(g_out));

    // Get a function handle for scheduling the stage
    if (g.output.stage_num > 0) {
        int stage_num = g.output.stage_num;
        f_handle = Func(g_out).update(stage_num - 1);
    } else {
        Func(g_out).compute_root();
        sched += f_handle.name() + ".compute_root()" + ";\n";
    }

    string var_prefix = g_out.name() + "_" +
                        std::to_string(g.output.stage_num);


    if (g.output.func.has_extern_definition()) {
        internal_assert(g.members.size() == 1);
        return sched;
    }

    // Realize tiling and update the dimension estimates
    vector<VarOrRVar> outer_dims;
    vector<VarOrRVar> inner_dims;

    vector<Dim> &dims = def.schedule().dims();
    internal_assert(dims.size() > 0);

    // Keep track of the rvars
    set<string> rvars;
    for (size_t d = 0; d < dims.size() - 1; d++) { // Ignore '__outermost'
        bool is_pure_var = false;
        for (const auto &arg : g_out.args()) {
            if (arg == get_base_name(dims[d].var)) {
                is_pure_var = true;
                break;
            }
        }
        if (!is_pure_var) {
            rvars.insert(get_base_name(dims[d].var));
        }
    }

    // Reorder the dimensions for better spatial locality
    map<string, int64_t> strides =
            analyze_spatial_locality(g.output, group_storage_bounds, inlines);
    reorder_dims(f_handle, def, strides, sched);

    vector<string> dim_vars;
    for (size_t d = 0; d < dims.size() - 1; d++) { // Ignore '__outermost'
        dim_vars.push_back(get_base_name(dims[d].var));
    }

    for (const auto &var : dim_vars) {
        bool is_rvar = (rvars.find(var) != rvars.end());
        VarOrRVar v(var, is_rvar);

        const auto &iter = g.tile_sizes.find(var);
        if ((iter != g.tile_sizes.end()) &&
            get_element(stg_estimates, var) != unknown &&
            get_element(stg_estimates, var) > iter->second) {
            int tile_size = iter->second;
            if (tile_size > 1) {
                pair<VarOrRVar, VarOrRVar> tile_vars =
                    split_dim(f_handle, v, tile_size, "_i", "_o", stg_estimates, sched);

                inner_dims.push_back(tile_vars.first);
                outer_dims.push_back(tile_vars.second);

                if (is_rvar) {
                    rvars.erase(var);
                    rvars.insert(tile_vars.first.name());
                    rvars.insert(tile_vars.second.name());
                }
            } else {
                outer_dims.push_back(v);
            }
        } else {
            inner_dims.push_back(v);
        }
    }

    // Reorder the tile dimensions
    if (outer_dims.size() > 0) {

        vector<VarOrRVar> ordering;
        for (const auto &v : inner_dims) {
            ordering.push_back(v);
        }
        for (const auto &v : outer_dims) {
            ordering.push_back(v);
        }

        string var_order = ordering[0].name();
        for (size_t o = 1; o < ordering.size(); o++) {
            var_order += ',' + ordering[o].name();
        }

        f_handle.reorder(ordering);
        sched += f_handle.name() + ".reorder(" + var_order + ");\n";
    }

    vectorize_stage(f_handle, def, g_out, t, rvars, stg_estimates, sched);

    // Parallelize definition
    uint32_t def_par = 1;
    // TODO: Investigate if it is better to pull one large dimension and
    // parallelize over it or generate nested parallelism.
    //
    // Go from the outer to the inner most loop till sufficient parallelism
    // is achieved.
    bool nested_parallelism = true;
    if (nested_parallelism) {
        int dim_start = dims.size() - 2;
        string seq_var = "";
        for (int d = dim_start; d >= 0; d--) {
            string var = get_base_name(dims[d].var);
            bool is_rvar = (rvars.find(var) != rvars.end());
            VarOrRVar v(var, is_rvar);

            if (is_rvar && !can_parallelize_rvar(var, g_out.name(), def)) {
                if (seq_var == "") {
                    seq_var = var;
                }
                continue;
            }

            if (def_par >= arch_params.parallelism) {
                // Enough parallelism to saturate target machine
                break;
            }

            const auto &iter = stg_estimates.find(var);
            if ((iter != stg_estimates.end()) && (iter->second != unknown)) {
                if (seq_var != "") {
                    VarOrRVar seq(seq_var, (rvars.find(seq_var) != rvars.end()));
                    f_handle.reorder(seq, v);
                    sched += f_handle.name() + ".reorder(" + seq_var + "," + var + ");\n";
                }
                f_handle.parallel(v);
                sched += f_handle.name() + ".parallel(" + var + ");\n";
                def_par *= iter->second;
            } else {
                break;
            }
        }
    }

    if (def_par < arch_params.parallelism) {
        user_warning << "Warning: insufficient parallelism for " << f_handle.name() << '\n';
    }

    // Find the level at which group members will be computed.
    int tile_inner_index = dims.size() - outer_dims.size() - 1;
    VarOrRVar tile_inner_var("", false);
    if (outer_dims.size() > 0) {
        string var_name = get_base_name(dims[tile_inner_index].var);
        bool is_rvar = (rvars.find(var_name) != rvars.end());
        tile_inner_var = VarOrRVar(var_name, is_rvar);
    }

    for (const FStage &mem : g.members) {
        // Skip member stages that have been inlined
        if (g.inlined.find(mem.func.name()) != g.inlined.end() ||
            mem.func.name() == g_out.name()) {
            continue;
        }

        // Get the definition corresponding to the stage
        Definition mem_def = get_stage_definition(mem.func, mem.stage_num);

        // Get the estimates for the dimensions of the member stage
        map<string, int64_t> mem_estimates = bounds_to_estimates(get_element(group_loop_bounds, mem));

        set<string> mem_rvars;
        const vector<Dim> &mem_dims = mem_def.schedule().dims();
        for (int d = 0; d < (int)mem_dims.size() - 1; d++) {
            bool is_pure_var = false;
            for (const auto &arg : mem.func.args()) {
                if (arg == get_base_name(mem_dims[d].var)) {
                    is_pure_var = true;
                    break;
                }
            }
            if (!is_pure_var) {
                mem_rvars.insert(get_base_name(mem_dims[d].var));
            }
        }

        // Get a function handle for scheduling the stage
        Stage mem_handle = Stage(Func(mem.func));

        if (mem.stage_num > 0) {
            mem_handle = Func(mem.func).update(mem.stage_num - 1);
        } else {
            if (outer_dims.size() > 0) {
                if (tile_inner_var.is_rvar) {
                    Func(mem.func).compute_at(Func(g_out), tile_inner_var.rvar);
                } else {
                    Func(mem.func).compute_at(Func(g_out), tile_inner_var.var);
                }
                sched += mem_handle.name() + ".compute_at(" + g_out.name() +
                        ',' + tile_inner_var.name() + ");\n";
            } else {
                user_warning << "Warning: Degenerate tiling no dimensions are tiled" << '\n';
                user_warning << "Computing " <<  mem.func.name() << " at root" << '\n';
                Func(mem.func).compute_root();
                sched += mem_handle.name() + ".compute_root()";
            }
        }

        // Reorder the dimensions for better spatial locality
        map<string, int64_t> mem_strides =
            analyze_spatial_locality(mem, group_storage_bounds, inlines);
        reorder_dims(mem_handle, mem_def, mem_strides, sched);

        vectorize_stage(mem_handle, mem_def, mem.func, t, mem_rvars,
                        mem_estimates, sched);
    }

    return sched;
}

/** Realizes the scheduling by following the grouping structure. Returns a
 * string representation of the schedule.
 *
 * TODO: A mode where schedules are not applied to the functions might be
 * interesting.
 *
 * TODO: The current form of the schedule returned is not very useful since it
 * cannot be manipulated and introspected very easily. The problem is that all
 * of the scheduling uses internal function and variable names which are not
 * visible to the user. Additionally, functions like sum and maximum are not
 * user visible. More thought needs to go into interaction between the user and
 * auto scheduling. */
string Partitioner::generate_cpu_schedule(const Target &t) {
    string sched = "";

    // Grab the group bounds early as they rely on the dimensions of the group
    // outputs which will be altered by modifying schedules.
    map<FStage, map<FStage, DimBounds>> loop_bounds = group_loop_bounds();
    map<FStage, map<string, Box>> storage_bounds = group_storage_bounds();

    set<string> inlines;
    // Mark all the functions that are Inlined.
    for (const pair<FStage, Group> &g : groups) {
        for (const string &inline_func : g.second.inlined) {
            inlines.insert(inline_func);
            Function f = get_element(dep_analysis.env, inline_func);
            Func f_handle(f);
            // TODO: Inlining functions with update definitions has different
            // behavior than pure functions. They may need to be computed above
            // the inner most vector loop to avoid complications with varying
            // extents across different vector lanes.
            f_handle.compute_inline();
            sched += f_handle.name() + ".compute_inline()" + ";\n";
        }
    }

    // Realize schedule for each group in the pipeline.
    for (const auto &g : groups) {
        sched += generate_group_cpu_schedule(g.second, t, loop_bounds[g.first],
                                             storage_bounds[g.first], inlines);
    }

    return sched;
}

/** Visitor to find all the variables the depend on a variable. */
class FindVarsUsingVar : public IRVisitor {
    using IRVisitor::visit;

    void visit(const Let *let) {
        if (expr_uses_vars(let->value, vars)) {
            vars.push(let->name, 0);
        }
        let->value.accept(this);
        let->body.accept(this);
    }
public :
    Scope<int> vars;

    FindVarsUsingVar(string var) {
        vars.push(var, 0);
    }
};

/** Returns the maximum stride a loop over var accesses the allocation 'func_acc'.
 * Access expressions along each dimension of the allocation are specified by
 * 'acc_exprs'. The dimensions of the allocation are specified by
 * 'buffer_bounds'. */
int64_t Partitioner::find_max_access_stride(const Scope<int> &vars,
                                            const string &func_acc,
                                            const vector<Expr> &acc_exprs,
                                            const Box &buffer_bounds) {
    size_t num_storage_dims = 0;
    int64_t bytes_per_ele = 0;

    // Get the number of dimensions of the allocated storage and the
    // number of bytes required to store a single value of func_acc.
    if (dep_analysis.env.find(func_acc) != dep_analysis.env.end()) {
        Function f = get_element(dep_analysis.env, func_acc);
        for (const auto &e : f.values()) {
            bytes_per_ele += e.type().bytes();
        }
        num_storage_dims = f.schedule().storage_dims().size();
    } else {
        bytes_per_ele = get_element(costs.inputs, func_acc).bytes();
        num_storage_dims = buffer_bounds.size();
    }

    int64_t curr_stride = bytes_per_ele;
    int64_t stride = 0;

    internal_assert(num_storage_dims <= acc_exprs.size());
    for (size_t sdim = 0; sdim < num_storage_dims; sdim++) {
        // Check if the access expression is dependent on the loop variable
        // var. Expressions that do not involve the variable have stride 0.
        if (expr_uses_vars(acc_exprs[sdim], vars)) {
           stride = std::max(stride, curr_stride);
        }

        Interval dim_range = buffer_bounds[sdim];
        int64_t dim_extent = get_extent(dim_range);
        curr_stride *= dim_extent;
    }

    return stride;
}

/** Returns the sum of access strides along each of the loop variables of a stage.
 * The bounds of all the allocations accessed is specified in allocation_bounds. */
map<string, int64_t>
Partitioner::analyze_spatial_locality(const FStage &stg,
                                      const map<string, Box> &allocation_bounds,
                                      const set<string> &inlines) {
    internal_assert(!stg.func.has_extern_definition());
    // Handle inlining. When a function is inlined into another the
    // stride of the accesses should be computed on the expression post inlining.
    // For example:
    // f(x, y) = ...;
    // g(x, y) = f(y, x); // transpose
    // h(x, y) = g(y, x); // transpose
    //
    // If both g and f are inlined into h then the resulting expression for h
    // will look like:
    // h(x, y) = f(x, y);
    //
    // Computing the stride of a loop over x in the function h will be incorrect
    // if inlining is not taken into account.

    // Get all the allocations accessed in the definition corresponding to stg.
    FindAllCalls find;
    Definition def = get_stage_definition(stg.func, stg.stage_num);
    // Perform inlining on the all the values and the args in the stage.
    for (size_t v = 0; v < def.values().size(); v++) {
        def.values()[v] = perform_inline(def.values()[v], dep_analysis.env,
                                         inlines);
    }

    for (size_t arg = 0; arg < def.args().size(); arg++) {
        def.args()[arg] = perform_inline(def.args()[arg], dep_analysis.env,
                                         inlines);
    }
    def.accept(&find);

    // Arguments on the left hand side might themselves involve accesses
    // to allocations and they need to be accounted for computing the strides
    // along each dimension.
    vector<pair<string, vector<Expr>>> call_args = find.call_args;
    // Account for the spatial locality of the store. Add the access on the
    // left hand side to call_args.
    vector<Expr> left_arg_exprs;
    for (size_t arg = 0; arg < def.args().size(); arg++) {
        left_arg_exprs.push_back(def.args()[arg]);
    }
    call_args.push_back(make_pair(stg.func.name(), left_arg_exprs));

    // Map for holding the strides across each dimension
    map<string, int64_t> var_strides;
    const vector<Dim> &dims = def.schedule().dims();

    for (size_t d = 0; d < dims.size() - 1; d++) {
        // Get all the variables involving the dimension in the definition.
        FindVarsUsingVar dep_vars(dims[d].var);
        def.accept(&dep_vars);

        // Accumulate the stride for each access for a loop dimension.
        int total_stride = 0;
        for (const pair<string, vector<Expr>> &call : call_args) {
            Box call_alloc_reg;
            if (allocation_bounds.find(call.first) != allocation_bounds.end()) {
                call_alloc_reg = get_element(allocation_bounds, call.first);
            } else {
                call_alloc_reg = get_element(pipeline_bounds, call.first);
            }
            total_stride += find_max_access_stride(dep_vars.vars, call.first,
                                                   call.second, call_alloc_reg);
        }
        var_strides[dims[d].var] = total_stride;
    }

    return var_strides;
}

// Verify that function 'f' does not have partially specified schedules/bounds.
// The current auto scheduler cannots handle such cases.
void validate_no_partial_schedules(const Function &f) {
    int num_stages = f.updates().size() + 1;
    for (int stage = 0; stage < num_stages; ++stage) {
        const Definition &def = get_stage_definition(f, stage);
        const Schedule &schedule = def.schedule();

        user_assert(schedule.splits().empty())
            << "AutoSchedule: cannot auto-schedule function \"" << f.name()
            << "\" since it has partially specified schedules at stage " << stage << "\n";
        user_assert(schedule.bounds().empty())
            << "AutoSchedule: cannot auto-schedule function \"" << f.name()
            << "\" since it has partially specified bounds at stage " << stage << "\n";

        // Verify that none of the dimensions are scheduled to be parallelized or
        // vectorized, or unrolled.
        for (const auto &d : schedule.dims()) {
            user_assert(d.for_type == ForType::Serial)
                << "AutoSchedule: cannot auto-schedule function \"" << f.name()
                << "\" since stage " << stage << " is not serial at dim " << d.var << "\n";
        }

        if (!f.has_extern_definition()) {
            if (stage == 0) {
                // Since we can only specialize on a Func, we only need to check for no
                // specializations for the initial stage.
                user_assert(def.specializations().empty())
                    << "AutoSchedule: cannot auto-schedule function \"" << f.name()
                    << "\" since it has specializations\n";

                // Verify that there is no loop reordering on the initial definition
                // (i.e. the Vars in the dim list should be in the same order as
                // the args in the LHS of the definition).
                internal_assert(schedule.dims().size() - 1 == def.args().size()); // Ignore '__outermost'
                for (size_t i = 0; i < def.args().size(); ++i) {
                    const Variable *arg = def.args()[i].as<Variable>();
                    internal_assert(arg);
                    user_assert(arg->name == schedule.dims()[i].var)
                        << "AutoSchedule: cannot auto-schedule function \"" << f.name()
                        << "\" since dim \"" << arg->name << "\" at stage " << stage
                        << " has been reordered\n";
                }
            } else {
                // Verify that there is no loop reordering on the update definition
                // (i.e. the Vars in the dim list should be in the same order as
                // the args in the LHS of the definition, the RVars in the dim list
                // should be in the same order as the RVars in the rvar list, and
                // all RVars should come before all Vars).

                // Ignore '__outermost' in 'dims'
                const vector<Dim> &dims = schedule.dims();
                const vector<ReductionVariable> &rvars = schedule.rvars();
                const vector<Expr> &args = f.definition().args();
                internal_assert(dims.size() - 1 >= rvars.size());

                for (size_t i = 0; i < rvars.size(); ++i) {
                    const Dim &d = dims[i];
                    user_assert(d.is_rvar() && (d.var == rvars[i].var))
                        << "AutoSchedule: cannot auto-schedule function \"" << f.name()
                        << "\" since dim \"" << i << "\" at stage " << stage
                        << " has been reordered\n";
                }

                internal_assert(dims.size() - rvars.size() - 1 <= args.size());
                int last_index = -1;
                for (size_t i = rvars.size(); i < dims.size() - 1; ++i) {
                    const Dim &d = dims[i];
                    user_assert(!d.is_rvar())
                        << "AutoSchedule: cannot auto-schedule function \"" << f.name()
                        << "\" since dim \"" << i << "\" at stage " << stage
                        << " has been reordered\n";

                    const auto &iter =
                        std::find_if(args.begin(), args.end(),
                                    [&d](const Expr &arg) {
                                        const Variable *v = arg.as<Variable>();
                                        return (d.var == v->name);
                                    });
                    internal_assert(iter != args.end());
                    int current_index = iter - args.begin();
                    user_assert(current_index > last_index)
                        << "AutoSchedule: cannot auto-schedule function \"" << f.name()
                        << "\" since dim \"" << i << "\" at stage " << stage
                        << " has been reordered\n";
                    last_index = current_index;
                }
            }
        }
    }
}

} // anonymous namespace

/** Generate schedules for all functions in the pipeline required to compute the
 * outputs. This applies the schedules and returns a string representation of
 * the schedules. The target architecture is specified by 'target'. */
string generate_schedules(const vector<Function> &outputs, const Target &target,
                          const MachineParams &arch_params) {
    string sched;
    // Make an environment map which is used throughout the auto scheduling process.
    map<string, Function> env;
    for (Function f : outputs) {
        map<string, Function> more_funcs = find_transitive_calls(f);
        env.insert(more_funcs.begin(), more_funcs.end());
    }

    // Validate that none of the functions in the pipeline have partial schedules.
    for (const auto &iter : env) {
        validate_no_partial_schedules(iter.second);
    }

    // Compute the bounds of function values which are used for dependence analysis.
    vector<string> order = realization_order(outputs, env);
    FuncValueBounds func_val_bounds = compute_function_value_bounds(order, env);

    // The auto scheduling algorithm requires estimates on the outputs of the
    // pipeline to get quantitative estimates of costs for computing functions
    // in the pipeline.
    bool estimates_avail = check_estimates_on_outputs(outputs);
    if (!estimates_avail) {
        user_warning << "Please provide estimates for each dimension "
                     << "of the pipeline output functions.\n";

        // Compute all the pipeline stages at root and store them at root.
        set_schedule_defaults(env);
        return sched;
    }

    DependenceAnalysis dep_analysis(env, func_val_bounds);

    // Compute bounds of all functions in the pipeline given estimates on
    // outputs. Also report functions which bounds could not be inferred.
    map<string, Box> pipeline_bounds = get_pipeline_bounds(dep_analysis, outputs);

    // Initialize the cost model.
    // Compute the expression costs for each function in the pipeline.
    RegionCosts costs(env);
    costs.disp_func_costs();

    Partitioner part(pipeline_bounds, arch_params, dep_analysis, costs, outputs);

    // Compute and display reuse
    /* TODO: Use the reuse estimates to reorder loops
    for (const auto &f : env) {
        FindAllCalls find;
        f.second.accept(&find);
        int num_stages = f.second.updates().size() + 1;
        for (int s = 0; s < num_stages; s++) {
            FStage curr_s(f.second, s);
            map<string, int64_t> reuse = part.evaluate_reuse(curr_s, find.funcs_called);
            debug(0) << curr_s << '\n';
            for (const auto &dir : reuse) {
                debug(0) << dir.first << " " << dir.second << ',';
            }
            debug(0) << '\n';
        }
    }*/

    // Display the current pipeline graph.
    // TODO: Output the graph in dot format.
    part.disp_pipeline_graph();
    part.disp_pipeline_bounds();

    part.initialize_groups();
    part.disp_pipeline_costs();

    part.group(Partitioner::INLINE);
    part.disp_grouping();

    part.grouping_cache.clear();
    part.group(Partitioner::FAST_MEM);

    part.disp_pipeline_costs();
    part.disp_grouping();
    part.disp_pipeline_graph();

    sched = part.generate_cpu_schedule(target);

    // TODO: Unify both inlining and grouping for fast mem
    // TODO: GPU scheduling
    // TODO: Hierarchical tiling

    return sched;
}

}
}