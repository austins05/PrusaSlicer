#include "ContourZ.hpp"

#include "AABBMesh.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "LayerRegion.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Slic3r::ContourZ {
namespace {

struct Params
{
    double min_z;
    double resolution;
    double max_segment_z_delta;
};

struct StitchParams
{
    double amplitude;
    double spacing;
    double min_foundation_z;
};

bool eligible_role(const ExtrusionRole role)
{
    return (role.is_solid_infill() || role == ExtrusionRole::Ironing) && ! role.is_bridge();
}

bool eligible_stitch_role(const ExtrusionRole role)
{
    if (role.is_bridge() || role.is_support() || role.is_skirt() || role == ExtrusionRole::WipeTower || role == ExtrusionRole::Ironing)
        return false;
    if (role.is_external())
        return false;
    return role.is_perimeter() || role.is_infill();
}

indexed_triangle_set transformed_mesh(const PrintObject &object)
{
    indexed_triangle_set mesh = object.model_object()->raw_indexed_triangle_set();
    const Transform3d trafo = object.trafo_centered();
    for (Vec3f &v : mesh.vertices)
        v = (trafo * v.cast<double>()).cast<float>();
    return mesh;
}

struct SurfaceDelta
{
    double delta;
    Vec3d normal;
};

std::optional<SurfaceDelta> surface_delta_at(const AABBMesh &mesh, const Point &point, const double print_z)
{
    const Vec3d origin{unscale<double>(point.x()), unscale<double>(point.y()), print_z};
    const auto up = mesh.query_ray_hit(origin, Vec3d::UnitZ());
    const auto down = mesh.query_ray_hit(origin, -Vec3d::UnitZ());

    const bool up_hit = up.is_hit();
    const bool down_hit = down.is_hit();
    if (! up_hit && ! down_hit)
        return std::nullopt;

    const double up_distance = up_hit ? up.distance() : std::numeric_limits<double>::infinity();
    const double down_distance = down_hit ? down.distance() : std::numeric_limits<double>::infinity();
    return up_distance <= down_distance ? SurfaceDelta{up_distance, up.normal()} : SurfaceDelta{-down_distance, down.normal()};
}

bool append_sample(const AABBMesh &mesh, const Layer &layer, const Params &params, const Point &point, Points &points, std::vector<coord_t> &z_offsets)
{
    const std::optional<SurfaceDelta> surface = surface_delta_at(mesh, point, layer.print_z);
    bool valid = surface.has_value() && surface->normal.z() >= 0.15;

    double delta = 0.;
    if (valid) {
        const double raw_delta = surface->delta;
        const double min_down = -(layer.height - params.min_z);
        const double max_up = params.min_z;
        valid = raw_delta >= min_down - 0.03 && raw_delta <= max_up + 0.03;
        if (valid)
            delta = std::clamp(raw_delta, min_down, max_up);
    }

    if (! z_offsets.empty()) {
        const double previous_delta = unscale<double>(z_offsets.back());
        delta = std::clamp(delta, previous_delta - params.max_segment_z_delta, previous_delta + params.max_segment_z_delta);
    }

    points.emplace_back(point);
    z_offsets.emplace_back(scaled<coord_t>(delta));
    return valid;
}

bool contour_path(const AABBMesh &mesh, const Layer &layer, const Params &params, ExtrusionPath &path)
{
    if (! path.z_offsets.empty())
        return false;
    if (! eligible_role(path.role()) || path.role().is_bridge() || path.size() < 2 || path.length() < scaled<double>(2.0))
        return false;
    if (layer.id() == 0 || layer.height <= params.min_z + EPSILON)
        return false;

    Points new_points;
    std::vector<coord_t> z_offsets;
    new_points.reserve(path.polyline.points.size());
    z_offsets.reserve(path.polyline.points.size());

    size_t valid_samples = 0;
    size_t total_samples = 0;
    for (size_t i = 0; i + 1 < path.polyline.points.size(); ++i) {
        const Point &a = path.polyline.points[i];
        const Point &b = path.polyline.points[i + 1];
        if (i == 0) {
            ++total_samples;
            if (append_sample(mesh, layer, params, a, new_points, z_offsets))
                ++valid_samples;
        }

        const double len = unscale<double>((b - a).cast<double>().norm());
        const int segments = std::max(1, int(std::ceil(len / params.resolution)));
        for (int s = 1; s <= segments; ++s) {
            const double t = double(s) / double(segments);
            const Point p = (a.cast<double>() + (b - a).cast<double>() * t).cast<coord_t>();
            ++total_samples;
            if (append_sample(mesh, layer, params, p, new_points, z_offsets))
                ++valid_samples;
        }
    }

    if (new_points.size() != z_offsets.size() || new_points.size() < 2)
        return false;
    if (valid_samples < 2 || valid_samples * 4 < total_samples * 3)
        return false;

    const bool has_nonzero_offset = std::any_of(z_offsets.begin(), z_offsets.end(), [](coord_t z) { return z != 0; });
    if (! has_nonzero_offset)
        return false;

    path.polyline.points = std::move(new_points);
    path.z_offsets = std::move(z_offsets);
    path.z_offsets_scale_extrusion = true;
    return true;
}

bool stitch_path(const Layer &layer, const StitchParams &params, ExtrusionPath &path)
{
    if (! path.z_offsets.empty())
        return false;
    if (! eligible_stitch_role(path.role()) || path.size() < 2)
        return false;
    if (layer.print_z - layer.height < params.min_foundation_z - EPSILON)
        return false;
    if (layer.print_z - std::clamp(params.amplitude, 0.0, 0.1) < params.min_foundation_z - EPSILON)
        return false;

    const double amplitude = std::clamp(params.amplitude, 0.0, 0.1);
    const double spacing = std::max(params.spacing, 1.25);
    if (amplitude <= 0.0 || spacing <= 0.0 || path.length() < scaled<double>(spacing * 2.0))
        return false;

    Points new_points;
    std::vector<coord_t> z_offsets;
    new_points.reserve(path.polyline.points.size() + size_t(std::ceil(unscale<double>(path.length()) / spacing)) + 1);
    z_offsets.reserve(new_points.capacity());

    auto offset_at_station = [amplitude](int station) {
        if (station <= 0)
            return 0.0;
        return station % 2 == 1 ? amplitude : -amplitude;
    };
    auto offset_at_distance = [spacing, offset_at_station](double d) {
        if (d <= 0.0)
            return 0.0;
        const int station = int(std::floor(d / spacing));
        const double local = (d - double(station) * spacing) / spacing;
        const double a = offset_at_station(station);
        const double b = offset_at_station(station + 1);
        return a + (b - a) * std::clamp(local, 0.0, 1.0);
    };

    double distance = 0.0;
    new_points.emplace_back(path.polyline.points.front());
    z_offsets.emplace_back(0);

    for (size_t i = 0; i + 1 < path.polyline.points.size(); ++i) {
        const Point &a = path.polyline.points[i];
        const Point &b = path.polyline.points[i + 1];
        const Vec2d delta = (b - a).cast<double>();
        const double len = unscale<double>(delta.norm());
        if (len <= EPSILON)
            continue;

        double next_stitch = (std::floor(distance / spacing) + 1.0) * spacing;
        while (next_stitch < distance + len - EPSILON) {
            const double t = (next_stitch - distance) / len;
            const Point p = (a.cast<double>() + (b - a).cast<double>() * t).cast<coord_t>();
            new_points.emplace_back(p);
            z_offsets.emplace_back(scaled<coord_t>(offset_at_distance(next_stitch)));
            next_stitch += spacing;
        }

        distance += len;
        new_points.emplace_back(b);
        if (i + 2 == path.polyline.points.size() && path.is_closed())
            z_offsets.emplace_back(0);
        else
            z_offsets.emplace_back(scaled<coord_t>(offset_at_distance(distance)));
    }

    if (new_points.size() != z_offsets.size() || new_points.size() < 3)
        return false;

    path.polyline.points = std::move(new_points);
    path.z_offsets = std::move(z_offsets);
    path.z_offsets_scale_extrusion = false;
    return true;
}

void contour_entity(const AABBMesh &mesh, const Layer &layer, const Params &params, ExtrusionEntity &entity)
{
    if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(&entity)) {
        for (ExtrusionEntity *child : collection->entities)
            contour_entity(mesh, layer, params, *child);
    } else if (auto *multipath = dynamic_cast<ExtrusionMultiPath *>(&entity)) {
        for (ExtrusionPath &path : multipath->paths)
            contour_path(mesh, layer, params, path);
    } else if (auto *path = dynamic_cast<ExtrusionPath *>(&entity)) {
        contour_path(mesh, layer, params, *path);
    }
}

void stitch_entity(const Layer &layer, const StitchParams &params, ExtrusionEntity &entity)
{
    if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(&entity)) {
        for (ExtrusionEntity *child : collection->entities)
            stitch_entity(layer, params, *child);
    } else if (auto *multipath = dynamic_cast<ExtrusionMultiPath *>(&entity)) {
        for (ExtrusionPath &path : multipath->paths)
            stitch_path(layer, params, path);
    } else if (auto *path = dynamic_cast<ExtrusionPath *>(&entity)) {
        stitch_path(layer, params, *path);
    }
}

void clear_entity(ExtrusionEntity &entity)
{
    if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(&entity)) {
        for (ExtrusionEntity *child : collection->entities)
            clear_entity(*child);
    } else if (auto *multipath = dynamic_cast<ExtrusionMultiPath *>(&entity)) {
        for (ExtrusionPath &path : multipath->paths) {
            path.z_offsets.clear();
            path.z_offsets_scale_extrusion = true;
        }
    } else if (auto *path = dynamic_cast<ExtrusionPath *>(&entity)) {
        path->z_offsets.clear();
        path->z_offsets_scale_extrusion = true;
    }
}

} // namespace

void contour_object(PrintObject &object)
{
    for (Layer *layer : object.layers())
        for (LayerRegion *region : layer->regions())
            for (ExtrusionEntity *entity : region->fills().entities)
                clear_entity(*entity);

    for (Layer *layer : object.layers())
        for (LayerRegion *region : layer->regions())
            for (ExtrusionEntity *entity : region->perimeters().entities)
                clear_entity(*entity);

    const PrintObjectConfig &config = object.config();
    if (! config.zaa_enabled && ! config.z_stitching)
        return;

    Params params{
        config.zaa_min_z.value,
        config.zaa_resolution.value,
        config.zaa_max_segment_z_delta.value
    };
    if (config.zaa_enabled && object.instances().size() == 1 && params.min_z > 0.0 && params.resolution > 0.0 && params.max_segment_z_delta > 0.0) {
        indexed_triangle_set mesh_data = transformed_mesh(object);
        AABBMesh mesh(mesh_data, false);

        for (Layer *layer : object.layers()) {
            for (LayerRegion *region : layer->regions()) {
                if (region->region().config().zaa_region_disable)
                    continue;
                for (ExtrusionEntity *entity : region->fills().entities)
                    contour_entity(mesh, *layer, params, *entity);
            }
        }
    }

    if (! config.z_stitching)
        return;

    StitchParams stitch_params{
        config.z_stitching_amplitude.value,
        config.z_stitching_spacing.value,
        config.z_stitching_min_foundation_z.value
    };

    if (stitch_params.amplitude <= 0.0 || stitch_params.spacing < 1.25 || stitch_params.min_foundation_z < 0.35)
        return;

    for (Layer *layer : object.layers()) {
        for (LayerRegion *region : layer->regions()) {
            if (region->region().config().zaa_region_disable)
                continue;
            for (ExtrusionEntity *entity : region->perimeters().entities)
                stitch_entity(*layer, stitch_params, *entity);
            for (ExtrusionEntity *entity : region->fills().entities)
                stitch_entity(*layer, stitch_params, *entity);
        }
    }
}

} // namespace Slic3r::ContourZ
