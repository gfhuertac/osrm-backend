#include "engine/plugins/plugin_base.hpp"
#include "engine/plugins/tile.hpp"

#include "util/coordinate_calculation.hpp"
#include "util/web_mercator.hpp"
#include "util/vector_tile.hpp"

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/geometries.hpp>
#include <boost/geometry/multi/geometries/multi_linestring.hpp>

#include <protozero/varint.hpp>
#include <protozero/pbf_writer.hpp>

#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <numeric>

#include <cmath>
#include <cstdint>

namespace osrm
{
namespace engine
{
namespace plugins
{
namespace detail
{
// Simple container class for WGS84 coordinates
template <typename T> struct Point final
{
    Point(T _x, T _y) : x(_x), y(_y) {}

    const T x;
    const T y;
};

// from mapnik-vector-tile
namespace pbf
{
inline unsigned encode_length(const unsigned len) { return (len << 3u) | 2u; }
}

struct BBox final
{
    BBox(const double _minx, const double _miny, const double _maxx, const double _maxy)
        : minx(_minx), miny(_miny), maxx(_maxx), maxy(_maxy)
    {
    }

    double width() const { return maxx - minx; }
    double height() const { return maxy - miny; }

    const double minx;
    const double miny;
    const double maxx;
    const double maxy;
};

// Simple container for integer coordinates (i.e. pixel coords)
struct point_type_i final
{
    point_type_i(std::int64_t _x, std::int64_t _y) : x(_x), y(_y) {}

    const std::int64_t x;
    const std::int64_t y;
};

struct TurnData final
{
    TurnData(std::size_t _in, std::size_t _out, std::size_t _weight)
        : in_angle_offset(_in), out_angle_offset(_out), weight_offset(_weight)
    {
    }

    const std::size_t in_angle_offset;
    const std::size_t out_angle_offset;
    const std::size_t weight_offset;
};

using FixedPoint = detail::Point<std::int32_t>;
using FloatPoint = detail::Point<double>;

using FixedLine = std::vector<FixedPoint>;
using FloatLine = std::vector<FloatPoint>;

typedef boost::geometry::model::point<double, 2, boost::geometry::cs::cartesian> point_t;
typedef boost::geometry::model::linestring<point_t> linestring_t;
typedef boost::geometry::model::box<point_t> box_t;
typedef boost::geometry::model::multi_linestring<linestring_t> multi_linestring_t;
const static box_t clip_box(point_t(-util::vector_tile::BUFFER, -util::vector_tile::BUFFER),
                            point_t(util::vector_tile::EXTENT + util::vector_tile::BUFFER,
                                    util::vector_tile::EXTENT + util::vector_tile::BUFFER));

// from mapnik-vector-tile
// Encodes a linestring using protobuf zigzag encoding
inline bool encodeLinestring(const FixedLine &line,
                             protozero::packed_field_uint32 &geometry,
                             std::int32_t &start_x,
                             std::int32_t &start_y)
{
    const std::size_t line_size = line.size();
    if (line_size < 2)
    {
        return false;
    }

    const unsigned line_to_length = static_cast<const unsigned>(line_size) - 1;

    auto pt = line.begin();
    geometry.add_element(9); // move_to | (1 << 3)
    geometry.add_element(protozero::encode_zigzag32(pt->x - start_x));
    geometry.add_element(protozero::encode_zigzag32(pt->y - start_y));
    start_x = pt->x;
    start_y = pt->y;
    geometry.add_element(detail::pbf::encode_length(line_to_length));
    for (++pt; pt != line.end(); ++pt)
    {
        const std::int32_t dx = pt->x - start_x;
        const std::int32_t dy = pt->y - start_y;
        geometry.add_element(protozero::encode_zigzag32(dx));
        geometry.add_element(protozero::encode_zigzag32(dy));
        start_x = pt->x;
        start_y = pt->y;
    }
    return true;
}

// from mapnik-vctor-tile
// Encodes a point
inline bool encodePoint(const FixedPoint &pt, protozero::packed_field_uint32 &geometry)
{
    geometry.add_element(9);
    const std::int32_t dx = pt.x;
    const std::int32_t dy = pt.y;
    // Manual zigzag encoding.
    geometry.add_element(protozero::encode_zigzag32(dx));
    geometry.add_element(protozero::encode_zigzag32(dy));
    return true;
}

FixedLine coordinatesToTileLine(const util::Coordinate start,
                                const util::Coordinate target,
                                const detail::BBox &tile_bbox)
{
    FloatLine geo_line;
    geo_line.emplace_back(static_cast<double>(util::toFloating(start.lon)),
                          static_cast<double>(util::toFloating(start.lat)));
    geo_line.emplace_back(static_cast<double>(util::toFloating(target.lon)),
                          static_cast<double>(util::toFloating(target.lat)));

    linestring_t unclipped_line;

    for (auto const &pt : geo_line)
    {
        double px_merc = pt.x * util::web_mercator::DEGREE_TO_PX;
        double py_merc = util::web_mercator::latToY(util::FloatLatitude(pt.y)) *
                         util::web_mercator::DEGREE_TO_PX;
        // convert lon/lat to tile coordinates
        const auto px = std::round(
            ((px_merc - tile_bbox.minx) * util::web_mercator::TILE_SIZE / tile_bbox.width()) *
            util::vector_tile::EXTENT / util::web_mercator::TILE_SIZE);
        const auto py = std::round(
            ((tile_bbox.maxy - py_merc) * util::web_mercator::TILE_SIZE / tile_bbox.height()) *
            util::vector_tile::EXTENT / util::web_mercator::TILE_SIZE);

        boost::geometry::append(unclipped_line, point_t(px, py));
    }

    multi_linestring_t clipped_line;

    boost::geometry::intersection(clip_box, unclipped_line, clipped_line);

    FixedLine tile_line;

    // b::g::intersection might return a line with one point if the
    // original line was very short and coords were dupes
    if (!clipped_line.empty() && clipped_line[0].size() == 2)
    {
        if (clipped_line[0].size() == 2)
        {
            for (const auto &p : clipped_line[0])
            {
                tile_line.emplace_back(p.get<0>(), p.get<1>());
            }
        }
    }

    return tile_line;
}

FixedPoint coordinatesToTilePoint(const util::Coordinate point, const detail::BBox &tile_bbox)
{
    const FloatPoint geo_point{static_cast<double>(util::toFloating(point.lon)),
                               static_cast<double>(util::toFloating(point.lat))};

    const double px_merc = geo_point.x * util::web_mercator::DEGREE_TO_PX;
    const double py_merc = util::web_mercator::latToY(util::FloatLatitude(geo_point.y)) *
                           util::web_mercator::DEGREE_TO_PX;

    const auto px = static_cast<std::int32_t>(std::round(
        ((px_merc - tile_bbox.minx) * util::web_mercator::TILE_SIZE / tile_bbox.width()) *
        util::vector_tile::EXTENT / util::web_mercator::TILE_SIZE));
    const auto py = static_cast<std::int32_t>(std::round(
        ((tile_bbox.maxy - py_merc) * util::web_mercator::TILE_SIZE / tile_bbox.height()) *
        util::vector_tile::EXTENT / util::web_mercator::TILE_SIZE));

    return FixedPoint{px, py};
}
}

Status TilePlugin::HandleRequest(const api::TileParameters &parameters, std::string &pbf_buffer)
{
    BOOST_ASSERT(parameters.IsValid());

    double min_lon, min_lat, max_lon, max_lat;

    // Convert the z,x,y mercator tile coordinates into WGS84 lon/lat values
    util::web_mercator::xyzToWGS84(parameters.x, parameters.y, parameters.z, min_lon, min_lat,
                                   max_lon, max_lat);

    util::Coordinate southwest{util::FloatLongitude(min_lon), util::FloatLatitude(min_lat)};
    util::Coordinate northeast{util::FloatLongitude(max_lon), util::FloatLatitude(max_lat)};

    // Fetch all the segments that are in our bounding box.
    // This hits the OSRM StaticRTree
    const auto edges = facade.GetEdgesInBox(southwest, northeast);

    std::vector<int> used_line_ints;
    std::unordered_map<int, std::size_t> line_int_offsets;
    uint8_t max_datasource_id = 0;

    std::vector<int> used_point_ints;
    std::unordered_map<int, std::size_t> point_int_offsets;
    std::vector<std::vector<detail::TurnData>> all_turn_data;

    const auto use_line_value = [&used_line_ints, &line_int_offsets](const int &value)
    {
        const auto found = line_int_offsets.find(value);

        if (found == line_int_offsets.end())
        {
            used_line_ints.push_back(value);
            line_int_offsets[value] = used_line_ints.size() - 1;
        }

        return;
    };

    const auto use_point_value = [&used_point_ints, &point_int_offsets](const int &value)
    {
        const auto found = point_int_offsets.find(value);
        std::size_t offset;

        if (found == point_int_offsets.end())
        {
            used_point_ints.push_back(value);
            offset = used_point_ints.size() - 1;
            point_int_offsets[value] = offset;
        }
        else
        {
            offset = found->second;
        }

        return offset;
    };

    // Loop over all edges once to tally up all the attributes we'll need.
    // We need to do this so that we know the attribute offsets to use
    // when we encode each feature in the tile.
    for (const auto &edge : edges)
    {
        int forward_weight = 0, reverse_weight = 0;
        uint8_t forward_datasource = 0;
        uint8_t reverse_datasource = 0;
        std::vector<detail::TurnData> edge_turn_data;
        // TODO this approach of writing at least an empty vector for any segment is probably stupid
        // (inefficient)

        if (edge.forward_packed_geometry_id != SPECIAL_EDGEID)
        {
            std::vector<EdgeWeight> forward_weight_vector;
            facade.GetUncompressedWeights(edge.forward_packed_geometry_id, forward_weight_vector);
            forward_weight = forward_weight_vector[edge.fwd_segment_position];

            std::vector<uint8_t> forward_datasource_vector;
            facade.GetUncompressedDatasources(edge.forward_packed_geometry_id,
                                              forward_datasource_vector);
            forward_datasource = forward_datasource_vector[edge.fwd_segment_position];

            use_line_value(forward_weight);

            std::vector<NodeID> forward_node_vector;
            facade.GetUncompressedGeometry(edge.forward_packed_geometry_id, forward_node_vector);

            // If this is the last segment on an edge (i.e. leads to an intersection), find outgoing
            // turns to write the turns point layer.
            if (edge.fwd_segment_position == forward_node_vector.size() - 1)
            {
                const auto sum_node_weight =
                    std::accumulate(forward_weight_vector.begin(), forward_weight_vector.end(), 0);

                // coord_a will be the OSM node immediately preceding the intersection, on the
                // current edge
                const auto coord_a = facade.GetCoordinateOfNode(
                    forward_node_vector.size() > 1
                        ? forward_node_vector[forward_node_vector.size() - 2]
                        : edge.u);
                // coord_b is the OSM intersection node, at the end of the current edge
                const auto coord_b = facade.GetCoordinateOfNode(edge.v);

                // There will often be multiple c_nodes. Here, we start by getting all outgoing
                // shortcuts, which we can whittle down (and deduplicate) to just the edges
                // immediately following intersections.
                // NOTE: the approach of only using shortcuts means that we aren't
                // getting or writing *every* turn here, but we don't especially care about turns
                // that will never be returned in a route anyway.
                std::unordered_map<NodeID, int> c_nodes;

                for (const auto adj_shortcut :
                     facade.GetAdjacentEdgeRange(edge.forward_segment_id.id))
                {
                    std::vector<contractor::QueryEdge::EdgeData> unpacked_shortcut;

                    // Outgoing shortcuts without `forward` travel enabled: do not want
                    if (!facade.GetEdgeData(adj_shortcut).forward)
                    {
                        continue;
                    }

                    routing_base.UnpackEdgeToEdges(edge.forward_segment_id.id,
                                                   facade.GetTarget(adj_shortcut),
                                                   unpacked_shortcut);

                    // Sometimes a "shortcut" is just an edge itself: this will not return a turn
                    if (unpacked_shortcut.size() < 2)
                    {
                        continue;
                    }

                    // Unpack the data from the second edge (the first edge will be the edge
                    // we're currently on), to use its geometry in calculating angle
                    const auto first_geometry_id =
                        facade.GetGeometryIndexForEdgeID(unpacked_shortcut[1].id);
                    std::vector<NodeID> first_geometry_vector;
                    facade.GetUncompressedGeometry(first_geometry_id, first_geometry_vector);

                    // EBE weight (the first edge in this shortcut) - EBN weight (calculated
                    // above by summing the distance of the current node-based edge) = turn weight
                    const auto sum_edge_weight = unpacked_shortcut[0].distance;
                    const auto turn_weight = sum_edge_weight - sum_node_weight;

                    c_nodes.emplace(first_geometry_vector.front(), turn_weight);
                }

                const uint64_t angle_in =
                    static_cast<uint64_t>(util::coordinate_calculation::bearing(coord_a, coord_b));

                // Only write for those that have angles out
                if (c_nodes.size() > 0)
                {
                    const auto angle_in_offset = use_point_value(angle_in);

                    for (const auto possible_next_node : c_nodes)
                    {
                        const auto coord_c = facade.GetCoordinateOfNode(possible_next_node.first);
                        const auto c_bearing = static_cast<uint64_t>(
                            util::coordinate_calculation::bearing(coord_b, coord_c));

                        const auto angle_out_offset = use_point_value(c_bearing);
                        const auto angle_weight_offset = use_point_value(possible_next_node.second);

                        // TODO this is not as efficient as it could be because of repeated
                        // angles_in
                        edge_turn_data.emplace_back(detail::TurnData{
                            angle_in_offset, angle_out_offset, angle_weight_offset});
                    }
                }
            }
        }

        if (edge.reverse_packed_geometry_id != SPECIAL_EDGEID)
        {
            std::vector<EdgeWeight> reverse_weight_vector;
            facade.GetUncompressedWeights(edge.reverse_packed_geometry_id, reverse_weight_vector);

            BOOST_ASSERT(edge.fwd_segment_position < reverse_weight_vector.size());

            reverse_weight =
                reverse_weight_vector[reverse_weight_vector.size() - edge.fwd_segment_position - 1];

            use_line_value(reverse_weight);

            std::vector<uint8_t> reverse_datasource_vector;
            facade.GetUncompressedDatasources(edge.reverse_packed_geometry_id,
                                              reverse_datasource_vector);
            reverse_datasource = reverse_datasource_vector[reverse_datasource_vector.size() -
                                                           edge.fwd_segment_position - 1];
        }
        // Keep track of the highest datasource seen so that we don't write unnecessary
        // data to the layer attribute values
        max_datasource_id = std::max(max_datasource_id, forward_datasource);
        max_datasource_id = std::max(max_datasource_id, reverse_datasource);

        all_turn_data.emplace_back(std::move(edge_turn_data));
    }

    // TODO: extract speed values for compressed and uncompressed geometries

    // Convert tile coordinates into mercator coordinates
    util::web_mercator::xyzToMercator(parameters.x, parameters.y, parameters.z, min_lon, min_lat,
                                      max_lon, max_lat);
    const detail::BBox tile_bbox{min_lon, min_lat, max_lon, max_lat};

    // Protobuf serializes blocks when objects go out of scope, hence
    // the extra scoping below.
    protozero::pbf_writer tile_writer{pbf_buffer};
    {
        {
            // Add a layer object to the PBF stream.  3=='layer' from the vector tile spec (2.1)
            protozero::pbf_writer line_layer_writer(tile_writer, util::vector_tile::LAYER_TAG);
            // TODO: don't write a layer if there are no features

            line_layer_writer.add_uint32(util::vector_tile::VERSION_TAG, 2); // version
            // Field 1 is the "layer name" field, it's a string
            line_layer_writer.add_string(util::vector_tile::NAME_TAG, "speeds"); // name
            // Field 5 is the tile extent.  It's a uint32 and should be set to 4096
            // for normal vector tiles.
            line_layer_writer.add_uint32(util::vector_tile::EXTENT_TAG,
                                         util::vector_tile::EXTENT); // extent

            // Begin the layer features block
            {
                // Each feature gets a unique id, starting at 1
                unsigned id = 1;
                for (const auto &edge : edges)
                {
                    // Get coordinates for start/end nodes of segment (NodeIDs u and v)
                    const auto a = facade.GetCoordinateOfNode(edge.u);
                    const auto b = facade.GetCoordinateOfNode(edge.v);
                    // Calculate the length in meters
                    const double length =
                        osrm::util::coordinate_calculation::haversineDistance(a, b);

                    int forward_weight = 0;
                    int reverse_weight = 0;

                    uint8_t forward_datasource = 0;
                    uint8_t reverse_datasource = 0;

                    if (edge.forward_packed_geometry_id != SPECIAL_EDGEID)
                    {
                        std::vector<EdgeWeight> forward_weight_vector;
                        facade.GetUncompressedWeights(edge.forward_packed_geometry_id,
                                                      forward_weight_vector);
                        forward_weight = forward_weight_vector[edge.fwd_segment_position];

                        std::vector<uint8_t> forward_datasource_vector;
                        facade.GetUncompressedDatasources(edge.forward_packed_geometry_id,
                                                          forward_datasource_vector);
                        forward_datasource = forward_datasource_vector[edge.fwd_segment_position];
                    }

                    if (edge.reverse_packed_geometry_id != SPECIAL_EDGEID)
                    {
                        std::vector<EdgeWeight> reverse_weight_vector;
                        facade.GetUncompressedWeights(edge.reverse_packed_geometry_id,
                                                      reverse_weight_vector);

                        BOOST_ASSERT(edge.fwd_segment_position < reverse_weight_vector.size());

                        reverse_weight = reverse_weight_vector[reverse_weight_vector.size() -
                                                               edge.fwd_segment_position - 1];

                        std::vector<uint8_t> reverse_datasource_vector;
                        facade.GetUncompressedDatasources(edge.reverse_packed_geometry_id,
                                                          reverse_datasource_vector);
                        reverse_datasource =
                            reverse_datasource_vector[reverse_datasource_vector.size() -
                                                      edge.fwd_segment_position - 1];
                    }

                    // Keep track of the highest datasource seen so that we don't write unnecessary
                    // data to the layer attribute values
                    max_datasource_id = std::max(max_datasource_id, forward_datasource);
                    max_datasource_id = std::max(max_datasource_id, reverse_datasource);

                    const auto encode_tile_line =
                        [&line_layer_writer, &edge, &id, &max_datasource_id](
                            const detail::FixedLine &tile_line, const std::uint32_t speed_kmh,
                            const std::size_t duration, const std::uint8_t datasource,
                            std::int32_t &start_x, std::int32_t &start_y)
                    {
                        // Here, we save the two attributes for our feature: the speed and the
                        // is_small
                        // boolean.  We only serve up speeds from 0-139, so all we do is save the
                        // first
                        protozero::pbf_writer feature_writer(line_layer_writer,
                                                             util::vector_tile::FEATURE_TAG);
                        // Field 3 is the "geometry type" field.  Value 2 is "line"
                        feature_writer.add_enum(
                            util::vector_tile::GEOMETRY_TAG,
                            util::vector_tile::GEOMETRY_TYPE_LINE); // geometry type
                        // Field 1 for the feature is the "id" field.
                        feature_writer.add_uint64(util::vector_tile::ID_TAG, id++); // id
                        {
                            // When adding attributes to a feature, we have to write
                            // pairs of numbers.  The first value is the index in the
                            // keys array (written later), and the second value is the
                            // index into the "values" array (also written later).  We're
                            // not writing the actual speed or bool value here, we're saving
                            // an index into the "values" array.  This means many features
                            // can share the same value data, leading to smaller tiles.
                            protozero::packed_field_uint32 field(
                                feature_writer, util::vector_tile::FEATURE_ATTRIBUTES_TAG);

                            field.add_element(0); // "speed" tag key offset
                            field.add_element(
                                std::min(speed_kmh, 127u)); // save the speed value, capped at 127
                            field.add_element(1);           // "is_small" tag key offset
                            field.add_element(128 +
                                              (edge.component.is_tiny ? 0 : 1)); // is_small feature
                            field.add_element(2);                // "datasource" tag key offset
                            field.add_element(130 + datasource); // datasource value offset
                            field.add_element(3);                // "duration" tag key offset
                            field.add_element(130 + max_datasource_id + 1 +
                                              duration); // duration value offset
                        }
                        {

                            // Encode the geometry for the feature
                            protozero::packed_field_uint32 geometry(
                                feature_writer, util::vector_tile::FEATURE_GEOMETRIES_TAG);
                            encodeLinestring(tile_line, geometry, start_x, start_y);
                        }
                    };

                    // If this is a valid forward edge, go ahead and add it to the tile
                    if (forward_weight != 0 && edge.forward_segment_id.enabled)
                    {
                        std::int32_t start_x = 0;
                        std::int32_t start_y = 0;

                        // Calculate the speed for this line
                        std::uint32_t speed_kmh =
                            static_cast<std::uint32_t>(round(length / forward_weight * 10 * 3.6));

                        auto tile_line = coordinatesToTileLine(a, b, tile_bbox);
                        if (!tile_line.empty())
                        {
                            encode_tile_line(tile_line, speed_kmh, line_int_offsets[forward_weight],
                                             forward_datasource, start_x, start_y);
                        }
                    }

                    // Repeat the above for the coordinates reversed and using the `reverse`
                    // properties
                    if (reverse_weight != 0 && edge.reverse_segment_id.enabled)
                    {
                        std::int32_t start_x = 0;
                        std::int32_t start_y = 0;

                        // Calculate the speed for this line
                        std::uint32_t speed_kmh =
                            static_cast<std::uint32_t>(round(length / reverse_weight * 10 * 3.6));

                        auto tile_line = coordinatesToTileLine(b, a, tile_bbox);
                        if (!tile_line.empty())
                        {
                            encode_tile_line(tile_line, speed_kmh, line_int_offsets[reverse_weight],
                                             reverse_datasource, start_x, start_y);
                        }
                    }
                }
            }

            // Field id 3 is the "keys" attribute
            // We need two "key" fields, these are referred to with 0 and 1 (their array indexes)
            // earlier
            line_layer_writer.add_string(util::vector_tile::KEY_TAG, "speed");
            line_layer_writer.add_string(util::vector_tile::KEY_TAG, "is_small");
            line_layer_writer.add_string(util::vector_tile::KEY_TAG, "datasource");
            line_layer_writer.add_string(util::vector_tile::KEY_TAG, "duration");

            // Now, we write out the possible speed value arrays and possible is_tiny
            // values.  Field type 4 is the "values" field.  It's a variable type field,
            // so requires a two-step write (create the field, then write its value)
            for (std::size_t i = 0; i < 128; i++)
            {
                // Writing field type 4 == variant type
                protozero::pbf_writer values_writer(line_layer_writer,
                                                    util::vector_tile::VARIANT_TAG);
                // Attribute value 5 == uint64 type
                values_writer.add_uint64(util::vector_tile::VARIANT_TYPE_UINT64, i);
            }
            {
                protozero::pbf_writer values_writer(line_layer_writer,
                                                    util::vector_tile::VARIANT_TAG);
                // Attribute value 7 == bool type
                values_writer.add_bool(util::vector_tile::VARIANT_TYPE_BOOL, true);
            }
            {
                protozero::pbf_writer values_writer(line_layer_writer,
                                                    util::vector_tile::VARIANT_TAG);
                // Attribute value 7 == bool type
                values_writer.add_bool(util::vector_tile::VARIANT_TYPE_BOOL, false);
            }
            for (std::size_t i = 0; i <= max_datasource_id; i++)
            {
                // Writing field type 4 == variant type
                protozero::pbf_writer values_writer(line_layer_writer,
                                                    util::vector_tile::VARIANT_TAG);
                // Attribute value 1 == string type
                values_writer.add_string(util::vector_tile::VARIANT_TYPE_STRING,
                                         facade.GetDatasourceName(i));
            }
            for (auto value : used_line_ints)
            {
                // Writing field type 4 == variant type
                protozero::pbf_writer values_writer(line_layer_writer,
                                                    util::vector_tile::VARIANT_TAG);
                // Attribute value 2 == float type
                // Durations come out of OSRM in integer deciseconds, so we convert them
                // to seconds with a simple /10 for display
                values_writer.add_double(util::vector_tile::VARIANT_TYPE_DOUBLE, value / 10.);
            }
        }

        {
            // Now write the points layer for turn penalty data:
            // Add a layer object to the PBF stream.  3=='layer' from the vector tile spec (2.1)
            protozero::pbf_writer point_layer_writer(tile_writer, util::vector_tile::LAYER_TAG);
            // TODO: don't write a layer if there are no features
            point_layer_writer.add_uint32(util::vector_tile::VERSION_TAG, 2); // version
            // Field 1 is the "layer name" field, it's a string
            point_layer_writer.add_string(util::vector_tile::NAME_TAG, "turns"); // name
            // Field 5 is the tile extent.  It's a uint32 and should be set to 4096
            // for normal vector tiles.
            point_layer_writer.add_uint32(util::vector_tile::EXTENT_TAG,
                                          util::vector_tile::EXTENT); // extent

            // Begin the layer features block
            {
                // Each feature gets a unique id, starting at 1
                unsigned id = 1;
                for (uint64_t i = 0; i < edges.size(); i++)
                {
                    const auto &edge = edges[i];
                    const auto &edge_turn_data = all_turn_data[i];

                    // Skip writing for edges with no turn penalty data
                    if (edge_turn_data.empty())
                    {
                        continue;
                    }

                    std::vector<NodeID> forward_node_vector;
                    facade.GetUncompressedGeometry(edge.forward_packed_geometry_id,
                                                   forward_node_vector);

                    // Skip writing for non-intersection segments
                    if (edge.fwd_segment_position != forward_node_vector.size() - 1)
                    {
                        continue;
                    }

                    const auto encode_tile_point =
                        [&point_layer_writer, &edge, &id](const detail::FixedPoint &tile_point,
                                                          const detail::TurnData &point_turn_data)
                    {
                        protozero::pbf_writer feature_writer(point_layer_writer,
                                                             util::vector_tile::FEATURE_TAG);
                        // Field 3 is the "geometry type" field.  Value 1 is "point"
                        feature_writer.add_enum(
                            util::vector_tile::GEOMETRY_TAG,
                            util::vector_tile::GEOMETRY_TYPE_POINT); // geometry type
                        // Field 1 for the feature is the "id" field.
                        feature_writer.add_uint64(util::vector_tile::ID_TAG, id++); // id
                        {
                            // See above for explanation
                            protozero::packed_field_uint32 field(
                                feature_writer, util::vector_tile::FEATURE_ATTRIBUTES_TAG);

                            field.add_element(0); // "bearing_in" tag key offset
                            field.add_element(point_turn_data.in_angle_offset);
                            field.add_element(1); // "bearing_out" tag key offset
                            field.add_element(point_turn_data.out_angle_offset);
                            field.add_element(2); // "weight" tag key offset
                            field.add_element(point_turn_data.weight_offset);
                        }
                        {
                            protozero::packed_field_uint32 geometry(
                                feature_writer, util::vector_tile::FEATURE_GEOMETRIES_TAG);
                            encodePoint(tile_point, geometry);
                        }
                    };

                    const auto turn_coordinate = facade.GetCoordinateOfNode(edge.v);
                    const auto tile_point = coordinatesToTilePoint(turn_coordinate, tile_bbox);

                    if (!boost::geometry::within(detail::point_t(tile_point.x, tile_point.y),
                                                 detail::clip_box))
                    {
                        continue;
                    }

                    for (const auto &individual_turn : edge_turn_data)
                    {
                        encode_tile_point(tile_point, individual_turn);
                    }
                }
            }

            // Field id 3 is the "keys" attribute
            // We need two "key" fields, these are referred to with 0 and 1 (their array indexes)
            // earlier
            point_layer_writer.add_string(util::vector_tile::KEY_TAG, "bearing_in");
            point_layer_writer.add_string(util::vector_tile::KEY_TAG, "bearing_out");
            point_layer_writer.add_string(util::vector_tile::KEY_TAG, "weight");

            // Now, we write out the possible integer values.
            for (const auto &value : used_point_ints)
            {
                // Writing field type 4 == variant type
                protozero::pbf_writer values_writer(point_layer_writer,
                                                    util::vector_tile::VARIANT_TAG);
                // Attribute value 5 == uint64 type
                values_writer.add_uint64(util::vector_tile::VARIANT_TYPE_UINT64, value);
            }
        }
    }

    return Status::Ok;
}
}
}
}
