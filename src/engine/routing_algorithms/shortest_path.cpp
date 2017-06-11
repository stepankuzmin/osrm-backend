#include "engine/routing_algorithms/shortest_path.hpp"
#include "engine/routing_algorithms/routing_base_ch.hpp"
#include "engine/routing_algorithms/routing_base_mld.hpp"
#include "engine/routing_algorithms/routing_init.hpp"

#include <boost/assert.hpp>
#include <boost/optional.hpp>
#include <memory>

namespace osrm
{
namespace engine
{
namespace routing_algorithms
{

namespace
{

// TODO: cleanup DO_NOT_FORCE_LOOP and search_{from,to}_{forward,reverse}_node flags
const static constexpr bool DO_NOT_FORCE_LOOP = false;

// allows a uturn at the target_phantom
// searches source forward/reverse -> target forward/reverse
template <typename Algorithm>
void searchWithUTurn(SearchEngineData<Algorithm> &engine_working_data,
                     const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                     typename SearchEngineData<Algorithm>::QueryHeap &forward_heap,
                     typename SearchEngineData<Algorithm>::QueryHeap &reverse_heap,
                     const bool search_from_forward_node,
                     const bool search_from_reverse_node,
                     const bool search_to_forward_node,
                     const bool search_to_reverse_node,
                     const PhantomNode &source_phantom,
                     const PhantomNode &target_phantom,
                     const EdgeWeight total_weight_to_forward,
                     const EdgeWeight total_weight_to_reverse,
                     EdgeWeight &new_total_weight,
                     std::vector<NodeID> &leg_packed_path)
{
    forward_heap.Clear();
    reverse_heap.Clear();

    auto single_node_path =
        insertNodesInHeaps(facade,
                           forward_heap,
                           reverse_heap,
                           {source_phantom, target_phantom},
                           search_from_forward_node ? total_weight_to_forward : INVALID_EDGE_WEIGHT,
                           search_from_reverse_node ? total_weight_to_reverse : INVALID_EDGE_WEIGHT,
                           search_to_forward_node,
                           search_to_reverse_node);

    search(engine_working_data,
           facade,
           forward_heap,
           reverse_heap,
           new_total_weight,
           leg_packed_path,
           false,
           false,
           {source_phantom, target_phantom},
           single_node_path.second);

    if (new_total_weight == INVALID_EDGE_WEIGHT && single_node_path.second != INVALID_EDGE_WEIGHT)
    {
        leg_packed_path.push_back(single_node_path.first);
        new_total_weight = single_node_path.second;
    }
}

// searches shortest path between:
// source forward/reverse -> target forward
// source forward/reverse -> target reverse
template <typename Algorithm>
void search(SearchEngineData<Algorithm> &engine_working_data,
            const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
            typename SearchEngineData<Algorithm>::QueryHeap &forward_heap,
            typename SearchEngineData<Algorithm>::QueryHeap &reverse_heap,
            const bool search_from_forward_node,
            const bool search_from_reverse_node,
            const bool search_to_forward_node,
            const bool search_to_reverse_node,
            const PhantomNode &source_phantom,
            const PhantomNode &target_phantom,
            const EdgeWeight total_weight_to_forward,
            const EdgeWeight total_weight_to_reverse,
            EdgeWeight &new_total_weight_to_forward,
            EdgeWeight &new_total_weight_to_reverse,
            std::vector<NodeID> &leg_packed_path_forward,
            std::vector<NodeID> &leg_packed_path_reverse)
{
    auto weight_to_forward =
        search_from_forward_node ? total_weight_to_forward : INVALID_EDGE_WEIGHT;
    auto weight_to_reverse =
        search_from_reverse_node ? total_weight_to_reverse : INVALID_EDGE_WEIGHT;

    if (search_to_forward_node)
    {
        forward_heap.Clear();
        reverse_heap.Clear();

        auto single_node_path = insertNodesInHeaps(facade,
                                                   forward_heap,
                                                   reverse_heap,
                                                   {source_phantom, target_phantom},
                                                   weight_to_forward,
                                                   weight_to_reverse,
                                                   true,
                                                   false);

        search(engine_working_data,
               facade,
               forward_heap,
               reverse_heap,
               new_total_weight_to_forward,
               leg_packed_path_forward,
               needsLoopForward(source_phantom, target_phantom),
               routing_algorithms::DO_NOT_FORCE_LOOP,
               {source_phantom, target_phantom},
               single_node_path.second);

        if (new_total_weight_to_forward == INVALID_EDGE_WEIGHT &&
            single_node_path.second != INVALID_EDGE_WEIGHT)
        {
            leg_packed_path_forward.push_back(single_node_path.first);
            new_total_weight_to_forward = single_node_path.second;
        }
    }

    if (search_to_reverse_node)
    {
        forward_heap.Clear();
        reverse_heap.Clear();

        auto single_node_path = insertNodesInHeaps(facade,
                                                   forward_heap,
                                                   reverse_heap,
                                                   {source_phantom, target_phantom},
                                                   weight_to_forward,
                                                   weight_to_reverse,
                                                   false,
                                                   true);

        search(engine_working_data,
               facade,
               forward_heap,
               reverse_heap,
               new_total_weight_to_reverse,
               leg_packed_path_reverse,
               routing_algorithms::DO_NOT_FORCE_LOOP,
               needsLoopBackwards(source_phantom, target_phantom),
               {source_phantom, target_phantom},
               single_node_path.second);

        if (new_total_weight_to_reverse == INVALID_EDGE_WEIGHT &&
            single_node_path.second != INVALID_EDGE_WEIGHT)
        {
            leg_packed_path_reverse.push_back(single_node_path.first);
            new_total_weight_to_reverse = single_node_path.second;
        }
    }
}

template <typename Algorithm>
void unpackLegs(const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                const std::vector<PhantomNodes> &phantom_nodes_vector,
                const std::vector<NodeID> &total_packed_path,
                const std::vector<std::size_t> &packed_leg_begin,
                const EdgeWeight shortest_path_weight,
                InternalRouteResult &raw_route_data)
{
    raw_route_data.unpacked_path_segments.resize(packed_leg_begin.size() - 1);

    raw_route_data.shortest_path_weight = shortest_path_weight;

    for (const auto current_leg : util::irange<std::size_t>(0UL, packed_leg_begin.size() - 1))
    {
        auto leg_begin = total_packed_path.begin() + packed_leg_begin[current_leg];
        auto leg_end = total_packed_path.begin() + packed_leg_begin[current_leg + 1];
        const auto &unpack_phantom_node_pair = phantom_nodes_vector[current_leg];
        unpackPath(facade,
                   leg_begin,
                   leg_end,
                   unpack_phantom_node_pair,
                   raw_route_data.unpacked_path_segments[current_leg]);

        raw_route_data.source_traversed_in_reverse.push_back(
            (*leg_begin != phantom_nodes_vector[current_leg].source_phantom.forward_segment_id.id));
        raw_route_data.target_traversed_in_reverse.push_back(
            (*std::prev(leg_end) !=
             phantom_nodes_vector[current_leg].target_phantom.forward_segment_id.id));
    }
}
}

template <typename Algorithm>
InternalRouteResult
shortestPathSearch(SearchEngineData<Algorithm> &engine_working_data,
                   const datafacade::ContiguousInternalMemoryDataFacade<Algorithm> &facade,
                   const std::vector<PhantomNodes> &phantom_nodes_vector,
                   const boost::optional<bool> continue_straight_at_waypoint)
{
    InternalRouteResult raw_route_data;
    raw_route_data.segment_end_coordinates = phantom_nodes_vector;
    const bool allow_uturn_at_waypoint =
        !(continue_straight_at_waypoint ? *continue_straight_at_waypoint
                                        : facade.GetContinueStraightDefault());

    engine_working_data.InitializeOrClearFirstThreadLocalStorage(facade.GetNumberOfNodes());

    auto &forward_heap = *engine_working_data.forward_heap_1;
    auto &reverse_heap = *engine_working_data.reverse_heap_1;

    bool search_from_forward_node =
        phantom_nodes_vector.front().source_phantom.forward_segment_id.enabled;
    bool search_from_reverse_node =
        phantom_nodes_vector.front().source_phantom.reverse_segment_id.enabled;

    EdgeWeight total_weight_to_forward = 0;
    EdgeWeight total_weight_to_reverse = 0;
    std::vector<NodeID> total_packed_path_to_forward;
    std::vector<NodeID> total_packed_path_to_reverse;
    std::vector<std::size_t> packed_leg_to_forward_begin;
    std::vector<std::size_t> packed_leg_to_reverse_begin;

    std::vector<NodeID> prev_packed_leg_to_forward;
    std::vector<NodeID> prev_packed_leg_to_reverse;

    std::size_t current_leg = 0;

    // this implements a dynamic program that finds the shortest route through a list of vias
    for (const auto &phantom_node_pair : phantom_nodes_vector)
    {
        EdgeWeight new_total_weight_to_forward = INVALID_EDGE_WEIGHT;
        EdgeWeight new_total_weight_to_reverse = INVALID_EDGE_WEIGHT;
        std::vector<NodeID> packed_leg_to_forward;
        std::vector<NodeID> packed_leg_to_reverse;

        const auto &source_phantom = phantom_node_pair.source_phantom;
        const auto &target_phantom = phantom_node_pair.target_phantom;

        bool search_to_forward_node = target_phantom.forward_segment_id.enabled;
        bool search_to_reverse_node = target_phantom.reverse_segment_id.enabled;

        BOOST_ASSERT(!search_from_forward_node || source_phantom.forward_segment_id.enabled);
        BOOST_ASSERT(!search_from_reverse_node || source_phantom.reverse_segment_id.enabled);

        if (search_to_reverse_node || search_to_forward_node)
        {
            if (allow_uturn_at_waypoint)
            {
                searchWithUTurn(engine_working_data,
                                facade,
                                forward_heap,
                                reverse_heap,
                                search_from_forward_node,
                                search_from_reverse_node,
                                search_to_forward_node,
                                search_to_reverse_node,
                                source_phantom,
                                target_phantom,
                                total_weight_to_forward,
                                total_weight_to_reverse,
                                new_total_weight_to_forward,
                                packed_leg_to_forward);
                // if only the reverse node is valid (e.g. when using the match plugin) we
                // actually need to move
                if (!target_phantom.forward_segment_id.enabled)
                {
                    BOOST_ASSERT(target_phantom.reverse_segment_id.enabled);
                    new_total_weight_to_reverse = new_total_weight_to_forward;
                    packed_leg_to_reverse = std::move(packed_leg_to_forward);
                    new_total_weight_to_forward = INVALID_EDGE_WEIGHT;

                    // (*)
                    //
                    //   Below we have to check if new_total_weight_to_forward is invalid.
                    //   This prevents use-after-move on packed_leg_to_forward.
                }
                else if (target_phantom.reverse_segment_id.enabled)
                {
                    new_total_weight_to_reverse = new_total_weight_to_forward;
                    packed_leg_to_reverse = packed_leg_to_forward;
                }
            }
            else
            {
                search(engine_working_data,
                       facade,
                       forward_heap,
                       reverse_heap,
                       search_from_forward_node,
                       search_from_reverse_node,
                       search_to_forward_node,
                       search_to_reverse_node,
                       source_phantom,
                       target_phantom,
                       total_weight_to_forward,
                       total_weight_to_reverse,
                       new_total_weight_to_forward,
                       new_total_weight_to_reverse,
                       packed_leg_to_forward,
                       packed_leg_to_reverse);
            }
        }

        // Note: To make sure we do not access the moved-from packed_leg_to_forward
        // we guard its access by a check for invalid edge weight. See  (*) above.

        // No path found for both target nodes?
        if ((INVALID_EDGE_WEIGHT == new_total_weight_to_forward) &&
            (INVALID_EDGE_WEIGHT == new_total_weight_to_reverse))
        {
            return raw_route_data;
        }

        // we need to figure out how the new legs connect to the previous ones
        if (current_leg > 0)
        {
            bool forward_to_forward =
                (new_total_weight_to_forward != INVALID_EDGE_WEIGHT) &&
                packed_leg_to_forward.front() == source_phantom.forward_segment_id.id;
            bool reverse_to_forward =
                (new_total_weight_to_forward != INVALID_EDGE_WEIGHT) &&
                packed_leg_to_forward.front() == source_phantom.reverse_segment_id.id;
            bool forward_to_reverse =
                (new_total_weight_to_reverse != INVALID_EDGE_WEIGHT) &&
                packed_leg_to_reverse.front() == source_phantom.forward_segment_id.id;
            bool reverse_to_reverse =
                (new_total_weight_to_reverse != INVALID_EDGE_WEIGHT) &&
                packed_leg_to_reverse.front() == source_phantom.reverse_segment_id.id;

            BOOST_ASSERT(!forward_to_forward || !reverse_to_forward);
            BOOST_ASSERT(!forward_to_reverse || !reverse_to_reverse);

            // in this case we always need to copy
            if (forward_to_forward && forward_to_reverse)
            {
                // in this case we copy the path leading to the source forward node
                // and change the case
                total_packed_path_to_reverse = total_packed_path_to_forward;
                packed_leg_to_reverse_begin = packed_leg_to_forward_begin;
                forward_to_reverse = false;
                reverse_to_reverse = true;
            }
            else if (reverse_to_forward && reverse_to_reverse)
            {
                total_packed_path_to_forward = total_packed_path_to_reverse;
                packed_leg_to_forward_begin = packed_leg_to_reverse_begin;
                reverse_to_forward = false;
                forward_to_forward = true;
            }
            BOOST_ASSERT(!forward_to_forward || !forward_to_reverse);
            BOOST_ASSERT(!reverse_to_forward || !reverse_to_reverse);

            // in this case we just need to swap to regain the correct mapping
            if (reverse_to_forward || forward_to_reverse)
            {
                total_packed_path_to_forward.swap(total_packed_path_to_reverse);
                packed_leg_to_forward_begin.swap(packed_leg_to_reverse_begin);
            }
        }

        if (new_total_weight_to_forward != INVALID_EDGE_WEIGHT)
        {
            BOOST_ASSERT(target_phantom.forward_segment_id.enabled);

            packed_leg_to_forward_begin.push_back(total_packed_path_to_forward.size());
            total_packed_path_to_forward.insert(total_packed_path_to_forward.end(),
                                                packed_leg_to_forward.begin(),
                                                packed_leg_to_forward.end());
            search_from_forward_node = true;
        }
        else
        {
            total_packed_path_to_forward.clear();
            packed_leg_to_forward_begin.clear();
            search_from_forward_node = false;
        }

        if (new_total_weight_to_reverse != INVALID_EDGE_WEIGHT)
        {
            BOOST_ASSERT(target_phantom.reverse_segment_id.enabled);

            packed_leg_to_reverse_begin.push_back(total_packed_path_to_reverse.size());
            total_packed_path_to_reverse.insert(total_packed_path_to_reverse.end(),
                                                packed_leg_to_reverse.begin(),
                                                packed_leg_to_reverse.end());
            search_from_reverse_node = true;
        }
        else
        {
            total_packed_path_to_reverse.clear();
            packed_leg_to_reverse_begin.clear();
            search_from_reverse_node = false;
        }

        prev_packed_leg_to_forward = std::move(packed_leg_to_forward);
        prev_packed_leg_to_reverse = std::move(packed_leg_to_reverse);

        total_weight_to_forward = new_total_weight_to_forward;
        total_weight_to_reverse = new_total_weight_to_reverse;

        ++current_leg;
    }

    BOOST_ASSERT(total_weight_to_forward != INVALID_EDGE_WEIGHT ||
                 total_weight_to_reverse != INVALID_EDGE_WEIGHT);

    // We make sure the fastest route is always in packed_legs_to_forward
    if (total_weight_to_forward < total_weight_to_reverse ||
        (total_weight_to_forward == total_weight_to_reverse &&
         total_packed_path_to_forward.size() < total_packed_path_to_reverse.size()))
    {
        // insert sentinel
        packed_leg_to_forward_begin.push_back(total_packed_path_to_forward.size());
        BOOST_ASSERT(packed_leg_to_forward_begin.size() == phantom_nodes_vector.size() + 1);

        unpackLegs(facade,
                   phantom_nodes_vector,
                   total_packed_path_to_forward,
                   packed_leg_to_forward_begin,
                   total_weight_to_forward,
                   raw_route_data);
    }
    else
    {
        // insert sentinel
        packed_leg_to_reverse_begin.push_back(total_packed_path_to_reverse.size());
        BOOST_ASSERT(packed_leg_to_reverse_begin.size() == phantom_nodes_vector.size() + 1);

        unpackLegs(facade,
                   phantom_nodes_vector,
                   total_packed_path_to_reverse,
                   packed_leg_to_reverse_begin,
                   total_weight_to_reverse,
                   raw_route_data);
    }

    return raw_route_data;
}

template InternalRouteResult
shortestPathSearch(SearchEngineData<ch::Algorithm> &engine_working_data,
                   const datafacade::ContiguousInternalMemoryDataFacade<ch::Algorithm> &facade,
                   const std::vector<PhantomNodes> &phantom_nodes_vector,
                   const boost::optional<bool> continue_straight_at_waypoint);

template InternalRouteResult
shortestPathSearch(SearchEngineData<corech::Algorithm> &engine_working_data,
                   const datafacade::ContiguousInternalMemoryDataFacade<corech::Algorithm> &facade,
                   const std::vector<PhantomNodes> &phantom_nodes_vector,
                   const boost::optional<bool> continue_straight_at_waypoint);

template InternalRouteResult
shortestPathSearch(SearchEngineData<mld::Algorithm> &engine_working_data,
                   const datafacade::ContiguousInternalMemoryDataFacade<mld::Algorithm> &facade,
                   const std::vector<PhantomNodes> &phantom_nodes_vector,
                   const boost::optional<bool> continue_straight_at_waypoint);

} // namespace routing_algorithms
} // namespace engine
} // namespace osrm
