#include "ArrangeHelper.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/MultipleBeds.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/ClipperUtils.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "boost/regex.hpp"
#include "boost/property_tree/json_parser.hpp"
#include "boost/algorithm/string/replace.hpp"
#include <boost/nowide/fstream.hpp>



namespace Slic3r {

static bool arrange_sequential_wipe_towers(const ConfigBase& config)
{
	return config.has("complete_objects") && config.opt_bool("complete_objects") &&
		   config.has("wipe_tower") && config.opt_bool("wipe_tower") &&
		   config.has("wipe_tower_arrange") && config.opt_bool("wipe_tower_arrange");
}

static bool has_custom_sequential_order(const Model& model)
{
	for (const ModelObject* mo : model.objects)
		for (const ModelInstance* mi : mo->instances)
			if (mi->sequential_print_order > 0)
				return true;
	return false;
}

static BoundingBox get_wipe_tower_box(const ConfigBase& config)
{
	double width = config.has("wipe_tower_width") ? config.opt_float("wipe_tower_width") : 5.;
	if (std::abs(width - 60.) < EPSILON)
		width = 5.;

	double depth = config.has("wipe_tower_depth") ? config.opt_float("wipe_tower_depth") : 15.;
	if (depth <= 0.)
		depth = 15.;

	const double brim = config.has("wipe_tower_brim_width") ? std::max(0., config.opt_float("wipe_tower_brim_width")) : 0.;

	return BoundingBox(
		Point::new_scale(-brim, -brim),
		Point::new_scale(width + brim, depth + brim));
}

static coord_t sequential_wipe_tower_gap(const ConfigBase& config)
{
	double gap = 1.;
	if (config.has("brim_width"))
		gap += std::max(0., config.opt_float("brim_width"));
	if (config.has("brim_separation"))
		gap += std::max(0., config.opt_float("brim_separation"));
	return scaled(gap);
}

static Polygon transformed_box_polygon(BoundingBox box, const Vec2crd& tr, double rotation)
{
	Polygon poly = box.polygon();
	if (std::abs(rotation) > EPSILON)
		poly.rotate(rotation);
	poly.translate(tr);
	return poly;
}

static double bbox_area(const BoundingBox& bb)
{
	if (!bb.defined)
		return std::numeric_limits<double>::max();

	const Vec2crd size = bb.size();
	return static_cast<double>(std::max<coord_t>(0, size.x())) *
		   static_cast<double>(std::max<coord_t>(0, size.y()));
}

static double bbox_perimeter(const BoundingBox& bb)
{
	if (!bb.defined)
		return std::numeric_limits<double>::max();

	const Vec2crd size = bb.size();
	return 2. * static_cast<double>(std::max<coord_t>(0, size.x()) + std::max<coord_t>(0, size.y()));
}

struct SequentialClearanceScoring {
	coord_t radius = 0;
	coord_t height = 0;

	bool enabled() const { return radius > 0 && height > 0; }
};

struct SequentialScheduleScore {
	size_t plate_count = 0;
	coord_t max_required_lift = 0;
	double total_required_lift = 0.;
	double compactness = 0.;
};

static double point_to_box_distance_sq(const Vec2crd& point, const BoundingBox& box)
{
	const double dx = point.x() < box.min.x() ? double(box.min.x() - point.x()) :
					  point.x() > box.max.x() ? double(point.x() - box.max.x()) : 0.;
	const double dy = point.y() < box.min.y() ? double(box.min.y() - point.y()) :
					  point.y() > box.max.y() ? double(point.y() - box.max.y()) : 0.;
	return dx * dx + dy * dy;
}

static bool segment_intersects_box(const Vec2crd& a, const Vec2crd& b, const BoundingBox& box)
{
	double t_min = 0.;
	double t_max = 1.;
	const double ax = double(a.x());
	const double ay = double(a.y());
	const double vx = double(b.x() - a.x());
	const double vy = double(b.y() - a.y());

	auto clip = [&t_min, &t_max](double p, double q) {
		if (std::abs(p) < EPSILON)
			return q >= 0.;
		const double t = q / p;
		if (p < 0.) {
			if (t > t_max)
				return false;
			if (t > t_min)
				t_min = t;
		} else {
			if (t < t_min)
				return false;
			if (t < t_max)
				t_max = t;
		}
		return true;
	};

	return clip(-vx, ax - double(box.min.x())) &&
		   clip(vx, double(box.max.x()) - ax) &&
		   clip(-vy, ay - double(box.min.y())) &&
		   clip(vy, double(box.max.y()) - ay);
}

static double segment_to_box_distance_sq(const Vec2crd& a, const Vec2crd& b, const BoundingBox& box)
{
	if (box.contains(a) || box.contains(b) || segment_intersects_box(a, b, box))
		return 0.;

	double best = std::min(point_to_box_distance_sq(a, box), point_to_box_distance_sq(b, box));
	const double ax = double(a.x());
	const double ay = double(a.y());
	const double bx = double(b.x());
	const double by = double(b.y());
	const double vx = bx - ax;
	const double vy = by - ay;
	const double len_sq = vx * vx + vy * vy;
	if (len_sq <= 0.)
		return best;

	const std::array<Vec2crd, 4> corners{{
		box.min,
		Vec2crd(box.max.x(), box.min.y()),
		box.max,
		Vec2crd(box.min.x(), box.max.y())
	}};
	for (const Vec2crd& corner : corners) {
		const double t = std::clamp(((double(corner.x()) - ax) * vx + (double(corner.y()) - ay) * vy) / len_sq, 0., 1.);
		const double px = ax + t * vx;
		const double py = ay + t * vy;
		const double dx = double(corner.x()) - px;
		const double dy = double(corner.y()) - py;
		best = std::min(best, dx * dx + dy * dy);
	}

	return best;
}

static SequentialClearanceScoring sequential_clearance_scoring(const ConfigBase& config)
{
	SequentialClearanceScoring out;
	if (config.has("extruder_clearance_radius"))
		out.radius = scaled(std::max(0., config.opt_float("extruder_clearance_radius")) + 5.);
	if (config.has("extruder_clearance_height"))
		out.height = scaled(std::max(0., config.opt_float("extruder_clearance_height")));
	return out;
}

static BoundingBox model_instance_local_box(const ModelObject& object, const ModelInstance& instance)
{
	BoundingBox bb;
	const TriangleMesh& raw_mesh = object.raw_mesh();
	Polygon pgn = its_convex_hull_2d_above(raw_mesh.its, instance.get_matrix_no_offset().cast<float>(), 0. - instance.get_offset().z());
	bb.merge(get_extents(pgn));
	return bb;
}

static std::optional<Vec2crd> optimal_sequential_wipe_tower_relative_pos(const Model& model, const ConfigBase& config)
{
	if (!arrange_sequential_wipe_towers(config))
		return std::nullopt;

	BoundingBox local_object_bb;
	for (const ModelObject* mo : model.objects) {
		for (const ModelInstance* mi : mo->instances) {
			if (!mi->printable)
				continue;

			BoundingBox candidate_bb = model_instance_local_box(*mo, *mi);
			if (!candidate_bb.defined)
				continue;

			if (!local_object_bb.defined || bbox_area(candidate_bb) > bbox_area(local_object_bb))
				local_object_bb = candidate_bb;
		}
	}

	if (!local_object_bb.defined)
		return std::nullopt;

	const double rotation = (M_PI / 180.) * model.wipe_tower().rotation;
	const BoundingBox tower_unrotated_bb = get_wipe_tower_box(config);
	const BoundingBox tower_local_bb = get_extents(transformed_box_polygon(tower_unrotated_bb, Vec2crd::Zero(), rotation));
	if (!tower_local_bb.defined)
		return std::nullopt;

	const coord_t gap = sequential_wipe_tower_gap(config);
	const Vec2crd object_center = local_object_bb.center();
	const Vec2crd tower_center  = tower_local_bb.center();

	const coord_t left_x   = local_object_bb.min.x() - gap - tower_local_bb.max.x();
	const coord_t right_x  = local_object_bb.max.x() + gap - tower_local_bb.min.x();
	const coord_t bottom_y = local_object_bb.min.y() - gap - tower_local_bb.max.y();
	const coord_t top_y    = local_object_bb.max.y() + gap - tower_local_bb.min.y();
	const coord_t center_x = object_center.x() - tower_center.x();
	const coord_t center_y = object_center.y() - tower_center.y();

	const std::array<Vec2crd, 8> candidates{{
		{right_x, center_y},
		{left_x, center_y},
		{center_x, top_y},
		{center_x, bottom_y},
		{right_x, top_y},
		{right_x, bottom_y},
		{left_x, top_y},
		{left_x, bottom_y}
	}};

	Vec2crd best_candidate = candidates.front();
	double best_score = std::numeric_limits<double>::max();

	for (const Vec2crd& candidate : candidates) {
		BoundingBox tower_bb = tower_local_bb;
		tower_bb.translate(candidate);

		BoundingBox combined_bb = local_object_bb;
		combined_bb.merge(tower_bb);

		double score = bbox_area(combined_bb);
		score += bbox_perimeter(combined_bb) * 0.01;
		const double dx = static_cast<double>(candidate.x() + tower_center.x() - object_center.x());
		const double dy = static_cast<double>(candidate.y() + tower_center.y() - object_center.y());
		score += (dx * dx + dy * dy) * 0.000001;

		if (tower_bb.overlap(local_object_bb))
			score += bbox_area(tower_bb) * 1000.;

		if (score < best_score) {
			best_score = score;
			best_candidate = candidate;
		}
	}

	return best_candidate;
}

static void attach_wipe_tower_footprint(Sequential::ObjectToPrint& object, const Polygon& wipe_tower_poly)
{
	if (wipe_tower_poly.points.empty() || object.pgns_at_height.empty())
		return;

	auto lowest = std::min_element(object.pgns_at_height.begin(), object.pgns_at_height.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });
	lowest->second = Geometry::convex_hull(Polygons{std::move(lowest->second), wipe_tower_poly});
}

	
static bool can_arrange_selected_bed(const Model& model, int bed_idx)
{
	// When arranging a single bed, all instances of each object present must be on the same bed.
	// Otherwise, the resulting order may not be possible to apply without messing up order
	// on the other beds.
	const auto map = s_multiple_beds.get_inst_map();
	for (const ModelObject* mo : model.objects) {
		std::map<int, bool> used_beds;
		bool mo_on_this_bed = false;
		for (const ModelInstance* mi : mo->instances) {
			int id = -1;
			if (auto it = map.find(mi->id()); it != map.end())
				id = it->second;
			if (id == bed_idx)
				mo_on_this_bed = true;
			used_beds[id] = true;
		}
		if (mo_on_this_bed && used_beds.size() != 1)
			return false;
	}
	return true;
}

static Sequential::PrinterGeometry get_printer_geometry(const ConfigBase& config)
{
	enum ShapeType {
		BOX,
		CONVEX
	};
	const coord_t sequential_safety_margin = scaled(5.);
	struct ExtruderSlice {
		coord_t height;
		ShapeType shape_type;
		std::vector<Polygon> polygons;
	};

	BuildVolume bv(config.opt<ConfigOptionPoints>("bed_shape")->values, 10.);
	const BoundingBox& bb = bv.bounding_box();
	Polygon bed_polygon;
	if (bv.type() == BuildVolume::Type::Circle) {
		// Generate an inscribed octagon.
		double r = bv.bounding_volume2d().size().x() / 2.;
		for (double a = 2*M_PI; a > 0.1; a -= M_PI/4.)
			bed_polygon.points.emplace_back(Point::new_scale(r * std::sin(a), r * std::cos(a)));
	} else {
		// Rectangle of Custom. Just use the bounding box.
		bed_polygon = bb.polygon();
	}

	std::vector<ExtruderSlice> slices;
	const std::string printer_notes = config.opt_string("printer_notes");
	{
		if (! printer_notes.empty()) {
			try {
				boost::nowide::ifstream in(resources_dir() + "/data/printer_gantries/geometries.json");
				boost::property_tree::ptree pt;
				boost::property_tree::read_json(in, pt);
				for (const auto& printer : pt.get_child("printers")) {
					slices = {};
					std::string printer_notes_match = printer.second.get<std::string>("printer_notes_regex");
					boost::regex rgx(printer_notes_match);
					if (! boost::regex_match(printer_notes, rgx))
						continue;

					for (const auto& obj : printer.second.get_child("slices")) {
						ExtruderSlice slice;
						slice.height = scaled(obj.second.get<double>("height"));
						std::string type_str = obj.second.get<std::string>("type");
						slice.shape_type = type_str == "box" ? BOX : CONVEX;
						for (const auto& polygon : obj.second.get_child("polygons")) {
							Polygon pgn;
							std::string pgn_str = polygon.second.data();
							boost::replace_all(pgn_str, ";", " ");
							boost::replace_all(pgn_str, ",", " ");
							std::stringstream ss(pgn_str);
							while (ss) {
								double x = 0.;
								double y = 0.;
								ss >> x >> y;
								if (ss)
									pgn.points.emplace_back(Point::new_scale(x, y));
							}
							if (! pgn.points.empty())
								slice.polygons.emplace_back(std::move(pgn));
						}
						slices.emplace_back(std::move(slice));
					}
					break;
				}
			}
			catch (const boost::property_tree::json_parser_error&) {
				// Failed to parse JSON. slices are empty, fallback will be used.
			}
		}
		if (slices.empty()) {
			// Fallback to primitive model using radius and height.
			coord_t r = scaled(std::max(0.1, config.opt_float("extruder_clearance_radius")));
			coord_t h = scaled(std::max(0.1, config.opt_float("extruder_clearance_height")));
			double bed_x = bv.bounding_volume2d().size().x();
			slices.push_back(ExtruderSlice{ 0, CONVEX, { { {  -5000000,   -5000000 }, {   5000000,   -5000000 }, {   5000000,   5000000 }, {  -5000000,   5000000 } } } });
			slices.push_back(ExtruderSlice{ 1000000, BOX, { { {  -r, -r }, { r, -r }, {   r,   r }, {  -r,  r } } } });
			slices.push_back(ExtruderSlice{ h, BOX, { { { -scaled(bed_x),  -r }, { scaled(bed_x),  -r }, { scaled(bed_x), r }, { -scaled(bed_x), r}}} });
		}
	}

	for (ExtruderSlice& slice : slices) {
		Polygons inflated;
		for (const Polygon& polygon : slice.polygons) {
			Polygons expanded = offset(polygon, float(sequential_safety_margin), jtSquare);
			if (expanded.empty())
				inflated.emplace_back(polygon);
			else
				append(inflated, expanded);
		}
		if (!inflated.empty()) {
			slice.polygons.clear();
			slice.polygons.reserve(inflated.size());
			for (Polygon& polygon : inflated)
				slice.polygons.emplace_back(std::move(polygon));
		}
	}

	// Convert the read data so libseqarrange understands them.
	Sequential::PrinterGeometry out;
	out.plate = bed_polygon;
	for (const ExtruderSlice& slice : slices) {
		(slice.shape_type == CONVEX ? out.convex_heights : out.box_heights).emplace(slice.height);
		out.extruder_slices.insert(std::make_pair(slice.height, slice.polygons));
	}
	return out;
}

static Sequential::SolverConfiguration get_solver_config(
	const Sequential::PrinterGeometry& printer_geometry,
	Sequential::DecimationPrecision decimation_precision = Sequential::SEQ_DECIMATION_PRECISION_LOW)
{
	Sequential::SolverConfiguration out(printer_geometry);
	out.set_DecimationPrecision(decimation_precision);
	return out;
}

static void tune_solver_config_for_arrange(Sequential::SolverConfiguration& solver_configuration, size_t object_count)
{
	if (object_count <= 8)
		solver_configuration.set_ObjectGroupSize(6);
	else if (object_count <= 12)
		solver_configuration.set_ObjectGroupSize(5);
}

static bool sequential_schedule_has_conflict(
	const Sequential::SolverConfiguration& solver_configuration,
	const Sequential::PrinterGeometry& printer_geometry,
	const std::vector<Sequential::ObjectToPrint>& objects,
	const std::vector<Sequential::ScheduledPlate>& plates)
{
	return Sequential::check_ScheduledObjectsForSequentialConflict(solver_configuration, printer_geometry, objects, plates).has_value();
}

static std::optional<Vec2d> sequential_conflict_point(const Sequential::SequentialConflict& conflict)
{
	return conflict.has_point ? std::optional<Vec2d>(unscale(conflict.point)) : std::nullopt;
}

static SequentialCollisionInfo make_sequential_collision_info(
	const Sequential::SequentialConflict& conflict,
	const Model& model)
{
	SequentialCollisionInfo out;
	out.point = sequential_conflict_point(conflict);
	for (const ModelObject* mo : model.objects)
		for (const ModelInstance* mi : mo->instances) {
			const int instance_id = int(mi->id().id);
			if (instance_id == conflict.first_id)
				out.hit_object = mo->name;
			if (instance_id == conflict.second_id)
				out.printing_object = mo->name;
		}
	return out;
}

static BoundingBox sequential_object_extents(const Sequential::ObjectToPrint& object)
{
	BoundingBox out;
	for (const auto& [height, polygon] : object.pgns_at_height)
		out.merge(get_extents(polygon));
	return out;
}

static const Sequential::ObjectToPrint* find_sequential_object(
	const std::vector<Sequential::ObjectToPrint>& objects,
	int id)
{
	auto it = std::find_if(objects.begin(), objects.end(), [id](const Sequential::ObjectToPrint& object) { return object.id == id; });
	return it == objects.end() ? nullptr : &*it;
}

static double sequential_object_area(const Sequential::ObjectToPrint& object)
{
	return bbox_area(sequential_object_extents(object));
}

static coord_t sequential_object_height(const Sequential::ObjectToPrint& object)
{
	return object.total_height;
}

static BoundingBox scheduled_plate_extents(
	const Sequential::ScheduledPlate& plate,
	const std::vector<Sequential::ObjectToPrint>& objects)
{
	BoundingBox out;
	for (const Sequential::ScheduledObject& scheduled : plate.scheduled_objects) {
		const Sequential::ObjectToPrint* object = find_sequential_object(objects, scheduled.id);
		if (object == nullptr)
			continue;

		BoundingBox bb = sequential_object_extents(*object);
		if (!bb.defined)
			continue;

		bb.translate(Point(scheduled.x, scheduled.y));
		out.merge(bb);
	}
	return out;
}

static double schedule_compactness_score(
	const std::vector<Sequential::ScheduledPlate>& plates,
	const std::vector<Sequential::ObjectToPrint>& objects)
{
	double score = 0.;
	for (const Sequential::ScheduledPlate& plate : plates) {
		BoundingBox extents = scheduled_plate_extents(plate, objects);
		if (extents.defined) {
			score += bbox_area(extents);
			score += bbox_perimeter(extents) * 1000.;
		}
	}
	return score;
}

static SequentialScheduleScore schedule_score(
	const std::vector<Sequential::ScheduledPlate>& plates,
	const std::vector<Sequential::ObjectToPrint>& objects,
	const SequentialClearanceScoring& clearance_scoring)
{
	SequentialScheduleScore out;
	out.plate_count = plates.size();
	out.compactness = schedule_compactness_score(plates, objects);

	if (!clearance_scoring.enabled())
		return out;

	const double radius_sq = double(clearance_scoring.radius) * double(clearance_scoring.radius);
	const coord_t lift_margin = scaled(0.5);

	for (const Sequential::ScheduledPlate& plate : plates) {
		struct CompletedObject {
			BoundingBox box;
			coord_t height = 0;
		};
		std::vector<CompletedObject> completed;
		std::optional<Vec2crd> previous_center;

		for (const Sequential::ScheduledObject& scheduled : plate.scheduled_objects) {
			const Sequential::ObjectToPrint* object = find_sequential_object(objects, scheduled.id);
			if (object == nullptr)
				continue;

			BoundingBox current_box = sequential_object_extents(*object);
			if (!current_box.defined)
				continue;
			current_box.translate(Point(scheduled.x, scheduled.y));

			const Vec2crd current_center = current_box.center();
			if (previous_center) {
				for (const CompletedObject& printed : completed) {
					if (segment_to_box_distance_sq(*previous_center, current_center, printed.box) <= radius_sq) {
						const coord_t required_lift = std::max<coord_t>(0, printed.height - clearance_scoring.height + lift_margin);
						out.max_required_lift = std::max(out.max_required_lift, required_lift);
						out.total_required_lift += double(required_lift);
					}
				}
			}

			completed.push_back({ current_box, sequential_object_height(*object) });
			previous_center = current_center;
		}
	}

	return out;
}

static bool is_better_score(const SequentialScheduleScore& candidate, const SequentialScheduleScore& current)
{
	if (current.plate_count == 0)
		return true;
	if (candidate.plate_count != current.plate_count)
		return candidate.plate_count < current.plate_count;
	if (candidate.max_required_lift != current.max_required_lift)
		return candidate.max_required_lift < current.max_required_lift;
	if (std::abs(candidate.total_required_lift - current.total_required_lift) > 1.)
		return candidate.total_required_lift < current.total_required_lift;
	return candidate.compactness < current.compactness;
}

static bool is_better_schedule(
	const std::vector<Sequential::ScheduledPlate>& candidate,
	const std::vector<Sequential::ScheduledPlate>& current,
	const std::vector<Sequential::ObjectToPrint>& objects,
	const SequentialClearanceScoring& clearance_scoring)
{
	return is_better_score(
		schedule_score(candidate, objects, clearance_scoring),
		schedule_score(current, objects, clearance_scoring));
}

static void append_unique_coord(std::vector<coord_t>& coords, coord_t value, coord_t min, coord_t max)
{
	if (value < min || value > max)
		return;
	if (std::find(coords.begin(), coords.end(), value) == coords.end())
		coords.emplace_back(value);
}

static std::vector<coord_t> sequential_axis_candidates(coord_t min, coord_t max)
{
	std::vector<coord_t> out;
	if (min > max)
		return out;

	append_unique_coord(out, min, min, max);
	append_unique_coord(out, max, min, max);
	append_unique_coord(out, min + (max - min) / 2, min, max);
	return out;
}

static void append_sequential_grid_candidates(std::vector<coord_t>& coords, coord_t min, coord_t max, coord_t step)
{
	if (min > max || step <= 0)
		return;
	for (coord_t value = min; value <= max; value += step)
		append_unique_coord(coords, value, min, max);
	append_unique_coord(coords, max, min, max);
}

static bool try_place_sequential_object(
	Sequential::ScheduledPlate& plate,
	const Sequential::ObjectToPrint& object,
	const BoundingBox& object_bb,
	const BoundingBox& bed_bb,
	const Sequential::SolverConfiguration& solver_configuration,
	const Sequential::PrinterGeometry& printer_geometry,
	const std::vector<Sequential::ObjectToPrint>& objects,
	const SequentialClearanceScoring& clearance_scoring,
	coord_t step,
	bool include_grid)
{
	const coord_t min_x = bed_bb.min.x() - object_bb.min.x();
	const coord_t max_x = bed_bb.max.x() - object_bb.max.x();
	const coord_t min_y = bed_bb.min.y() - object_bb.min.y();
	const coord_t max_y = bed_bb.max.y() - object_bb.max.y();
	if (min_x > max_x || min_y > max_y)
		return false;

	std::vector<coord_t> xs = sequential_axis_candidates(min_x, max_x);
	std::vector<coord_t> ys = sequential_axis_candidates(min_y, max_y);

	for (const Sequential::ScheduledObject& scheduled : plate.scheduled_objects) {
		const Sequential::ObjectToPrint* placed_object = find_sequential_object(objects, scheduled.id);
		if (placed_object == nullptr)
			continue;

		BoundingBox placed_bb = sequential_object_extents(*placed_object);
		if (!placed_bb.defined)
			continue;
		placed_bb.translate(Point(scheduled.x, scheduled.y));

		const std::array<coord_t, 4> gaps{ 0, scaled(0.5), step, step * 2 };
		for (coord_t gap : gaps) {
			append_unique_coord(xs, placed_bb.max.x() - object_bb.min.x() + gap, min_x, max_x);
			append_unique_coord(xs, placed_bb.min.x() - object_bb.max.x() - gap, min_x, max_x);
			append_unique_coord(ys, placed_bb.max.y() - object_bb.min.y() + gap, min_y, max_y);
			append_unique_coord(ys, placed_bb.min.y() - object_bb.max.y() - gap, min_y, max_y);
		}
	}

	if (include_grid) {
		append_sequential_grid_candidates(xs, min_x, max_x, step);
		append_sequential_grid_candidates(ys, min_y, max_y, step);
	}

	const size_t max_attempts = include_grid ? 20000 : 12000;
	size_t attempts = 0;
	std::optional<Sequential::ScheduledObject> best_object;
	SequentialScheduleScore best_score;
	for (coord_t y : ys) {
		for (coord_t x : xs) {
			if (++attempts > max_attempts)
				break;
			plate.scheduled_objects.emplace_back(object.id, x, y);
			if (!sequential_schedule_has_conflict(solver_configuration, printer_geometry, objects, std::vector<Sequential::ScheduledPlate>{plate})) {
				const SequentialScheduleScore score = schedule_score(std::vector<Sequential::ScheduledPlate>{plate}, objects, clearance_scoring);
				if (is_better_score(score, best_score)) {
					best_score = score;
					best_object = plate.scheduled_objects.back();
				}
			}
			plate.scheduled_objects.pop_back();
		}
		if (attempts > max_attempts)
			break;
	}

	if (!best_object)
		return false;

	plate.scheduled_objects.emplace_back(*best_object);
	return true;
}

static std::vector<Sequential::ScheduledPlate> greedy_schedule_in_fixed_order(
	const Sequential::SolverConfiguration& solver_configuration,
	const Sequential::PrinterGeometry& printer_geometry,
	const std::vector<Sequential::ObjectToPrint>& objects,
	const SequentialClearanceScoring& clearance_scoring)
{
	BoundingBox bed_bb = get_extents(printer_geometry.plate);
	if (!bed_bb.defined)
		bed_bb = solver_configuration.plate_bounding_box;

	const std::array<std::pair<coord_t, bool>, 3> search_passes{ {
		{ scaled(5.), true },
		{ scaled(2.), false },
		{ scaled(1.), false }
	} };
	std::vector<Sequential::ScheduledPlate> plates(1);

	for (const Sequential::ObjectToPrint& object : objects) {
		BoundingBox object_bb = sequential_object_extents(object);
		if (!object_bb.defined) {
			plates.back().scheduled_objects.emplace_back(object.id, 0, 0);
			continue;
		}

		bool placed = false;
		for (const auto& [step, include_grid] : search_passes)
			for (Sequential::ScheduledPlate& plate : plates)
				if (!placed)
					placed = try_place_sequential_object(plate, object, object_bb, bed_bb, solver_configuration, printer_geometry, objects, clearance_scoring, step, include_grid);

		if (!placed) {
			Sequential::ScheduledPlate new_plate;
			if (!try_place_sequential_object(new_plate, object, object_bb, bed_bb, solver_configuration, printer_geometry, objects, clearance_scoring, search_passes.front().first, true))
				new_plate.scheduled_objects.emplace_back(object.id, 0, 0);
			plates.emplace_back(std::move(new_plate));
		}
	}

	return plates;
}

using SequentialObjectChain = std::vector<Sequential::ObjectToPrint>;

static std::vector<SequentialObjectChain> sequential_object_chains(const std::vector<Sequential::ObjectToPrint>& objects)
{
	std::vector<SequentialObjectChain> chains;
	SequentialObjectChain chain;
	for (const Sequential::ObjectToPrint& object : objects) {
		chain.emplace_back(object);
		if (!object.glued_to_next) {
			chains.emplace_back(std::move(chain));
			chain = {};
		}
	}
	if (!chain.empty())
		chains.emplace_back(std::move(chain));
	return chains;
}

static std::vector<Sequential::ObjectToPrint> flatten_object_chains(const std::vector<SequentialObjectChain>& chains)
{
	std::vector<Sequential::ObjectToPrint> out;
	for (const SequentialObjectChain& chain : chains)
		for (const Sequential::ObjectToPrint& object : chain)
			out.emplace_back(object);
	return out;
}

static double chain_area_score(const SequentialObjectChain& chain)
{
	double out = 0.;
	for (const Sequential::ObjectToPrint& object : chain)
		out += sequential_object_area(object);
	return out;
}

static coord_t chain_height_score(const SequentialObjectChain& chain)
{
	coord_t out = 0;
	for (const Sequential::ObjectToPrint& object : chain)
		out = std::max(out, sequential_object_height(object));
	return out;
}

static std::vector<Sequential::ScheduledPlate> greedy_schedule_best_effort(
	const Sequential::SolverConfiguration& solver_configuration,
	const Sequential::PrinterGeometry& printer_geometry,
	const std::vector<Sequential::ObjectToPrint>& objects,
	const SequentialClearanceScoring& clearance_scoring,
	bool allow_reorder)
{
	std::vector<std::vector<SequentialObjectChain>> variants;
	variants.emplace_back(sequential_object_chains(objects));

	if (allow_reorder && variants.front().size() > 1) {
		std::vector<SequentialObjectChain> by_area = variants.front();
		std::stable_sort(by_area.begin(), by_area.end(), [](const SequentialObjectChain& lhs, const SequentialObjectChain& rhs) {
			return chain_area_score(lhs) > chain_area_score(rhs);
		});
		variants.emplace_back(std::move(by_area));

		std::vector<SequentialObjectChain> by_height = variants.front();
		std::stable_sort(by_height.begin(), by_height.end(), [](const SequentialObjectChain& lhs, const SequentialObjectChain& rhs) {
			return chain_height_score(lhs) > chain_height_score(rhs);
		});
		variants.emplace_back(std::move(by_height));

		std::vector<SequentialObjectChain> by_footprint_then_height = variants.front();
		std::stable_sort(by_footprint_then_height.begin(), by_footprint_then_height.end(), [](const SequentialObjectChain& lhs, const SequentialObjectChain& rhs) {
			const double lhs_area = chain_area_score(lhs);
			const double rhs_area = chain_area_score(rhs);
			if (std::abs(lhs_area - rhs_area) > 1.)
				return lhs_area > rhs_area;
			return chain_height_score(lhs) > chain_height_score(rhs);
		});
		variants.emplace_back(std::move(by_footprint_then_height));
	}

	std::vector<Sequential::ScheduledPlate> best;
	for (const std::vector<SequentialObjectChain>& variant : variants) {
		std::vector<Sequential::ObjectToPrint> ordered_objects = flatten_object_chains(variant);
		std::vector<Sequential::ScheduledPlate> candidate =
			greedy_schedule_in_fixed_order(solver_configuration, printer_geometry, ordered_objects, clearance_scoring);
		if (sequential_schedule_has_conflict(solver_configuration, printer_geometry, ordered_objects, candidate))
			continue;
		if (is_better_schedule(candidate, best, ordered_objects, clearance_scoring))
			best = std::move(candidate);
	}

	if (!best.empty())
		return best;
	return greedy_schedule_in_fixed_order(solver_configuration, printer_geometry, objects, clearance_scoring);
}

static std::vector<Sequential::ObjectToPrint> get_objects_to_print(
	const Model& model,
	const Sequential::PrinterGeometry& printer_geometry,
	int selected_bed,
	const std::optional<Vec2crd>& wipe_tower_relative_pos,
	const ConfigBase& config)
{
	// First extract the heights of interest.
	std::vector<double> heights;
	for (const auto& [height, pgns] : printer_geometry.extruder_slices)
		heights.push_back(unscaled(height));
	Slic3r::sort_remove_duplicates(heights);

	// Now collect all objects and projections of convex hull above respective heights.
	std::vector<std::pair<Sequential::ObjectToPrint, std::vector<Sequential::ObjectToPrint>>> objects; // first = object id, the vector = ids of its instances
	std::map<int, int> sequential_order_by_id;
	std::vector<int> model_order_ids;
	for (const ModelObject* mo : model.objects)
		for (const ModelInstance* mi : mo->instances) {
			sequential_order_by_id[int(mi->id().id)] = mi->sequential_print_order;
			if (selected_bed != -1) {
				auto it = s_multiple_beds.get_inst_map().find(mi->id());
				if (it == s_multiple_beds.get_inst_map().end() || it->second != selected_bed)
					continue;
			}
			if (mi->printable)
				model_order_ids.emplace_back(int(mi->id().id));
		}

	std::map<int, int> sequential_rank_by_id;
	if (std::any_of(model_order_ids.begin(), model_order_ids.end(), [&sequential_order_by_id](int id) { return sequential_order_by_id[id] > 0; })) {
		std::vector<int> explicit_order;
		for (int id : model_order_ids)
			if (sequential_order_by_id[id] > 0)
				explicit_order.emplace_back(id);

		std::stable_sort(explicit_order.begin(), explicit_order.end(), [&sequential_order_by_id](int lhs_id, int rhs_id) {
			return sequential_order_by_id[lhs_id] < sequential_order_by_id[rhs_id];
		});

		std::vector<int> ordered(model_order_ids.size(), -1);
		std::vector<int> placed;
		placed.reserve(explicit_order.size());
		for (int id : explicit_order) {
			size_t target = size_t(std::min<int>(int(model_order_ids.size()) - 1, sequential_order_by_id[id] - 1));
			while (target < ordered.size() && ordered[target] != -1)
				++target;
			if (target == ordered.size()) {
				target = 0;
				while (target < ordered.size() && ordered[target] != -1)
					++target;
			}
			if (target < ordered.size()) {
				ordered[target] = id;
				placed.emplace_back(id);
			}
		}

		size_t fill_idx = 0;
		for (int id : model_order_ids) {
			if (std::find(placed.begin(), placed.end(), id) != placed.end())
				continue;
			while (fill_idx < ordered.size() && ordered[fill_idx] != -1)
				++fill_idx;
			if (fill_idx < ordered.size())
				ordered[fill_idx] = id;
		}

		for (size_t rank = 0; rank < ordered.size(); ++rank)
			if (ordered[rank] != -1)
				sequential_rank_by_id[ordered[rank]] = int(rank);
	}

	auto order_less = [&sequential_rank_by_id](int lhs_id, int rhs_id) {
		auto lhs_rank = sequential_rank_by_id.find(lhs_id);
		auto rhs_rank = sequential_rank_by_id.find(rhs_id);
		if (lhs_rank != sequential_rank_by_id.end() && rhs_rank != sequential_rank_by_id.end())
			return lhs_rank->second < rhs_rank->second;
		return lhs_id < rhs_id;
	};

	for (const ModelObject* mo : model.objects) {
		const TriangleMesh& raw_mesh = mo->raw_mesh();
		coord_t height = scaled(mo->instance_bounding_box(0).size().z());
		std::vector<Sequential::ObjectToPrint> instances;
		for (const ModelInstance* mi : mo->instances) {
			if (selected_bed != -1) {
				auto it = s_multiple_beds.get_inst_map().find(mi->id());
				if (it == s_multiple_beds.get_inst_map().end() || it->second != selected_bed)
					continue;
			}
			if (mi->printable) {
				instances.emplace_back(Sequential::ObjectToPrint{int(mi->id().id), true, height, {}});

				for (double height : heights) {
					// It seems that zero level in the object instance is mi->get_offset().z(), however need to have bed as zero level,
					// hence substracting mi->get_offset().z() from height seems to be an easy hack
					Polygon pgn = its_convex_hull_2d_above(raw_mesh.its, mi->get_matrix_no_offset().cast<float>(), height - mi->get_offset().z());
					instances.back().pgns_at_height.emplace_back(std::make_pair(scaled(height), pgn));
				}

				if (wipe_tower_relative_pos) {
					const double rotation = (M_PI / 180.) * model.wipe_tower().rotation;
					Polygon wipe_tower_poly = transformed_box_polygon(get_wipe_tower_box(config), *wipe_tower_relative_pos, rotation);
					attach_wipe_tower_footprint(instances.back(), wipe_tower_poly);
				}
			}
		}
		
		// Collect all instances of this object to be arranged, unglue it from the next object.
		if (! instances.empty()) {
			std::stable_sort(instances.begin(), instances.end(), [&order_less](const auto& a, const auto& b) {
				return order_less(a.id, b.id);
			});
			objects.emplace_back(instances.front(), instances);
			objects.back().second.erase(objects.back().second.begin()); // pop_front
			if (! objects.back().second.empty())
				objects.back().second.back().glued_to_next = false;
			else
				objects.back().first.glued_to_next = false;
		}
	}

	// Now order the objects so that they are deterministic. User-defined sequential
	// order takes priority, while instances of the same model object stay grouped.
	auto group_rank = [&sequential_rank_by_id](const auto& group) {
		int rank = -1;
		auto update_rank = [&sequential_rank_by_id, &rank](const Sequential::ObjectToPrint& object) {
			auto it = sequential_rank_by_id.find(object.id);
			if (it != sequential_rank_by_id.end())
				rank = rank == -1 ? it->second : std::min(rank, it->second);
		};
		update_rank(group.first);
		for (const Sequential::ObjectToPrint& instance : group.second)
			update_rank(instance);
		return rank;
	};
	std::stable_sort(objects.begin(), objects.end(), [&group_rank](const auto& a, const auto& b) {
		const int a_rank = group_rank(a);
		const int b_rank = group_rank(b);
		if (a_rank >= 0 && b_rank >= 0)
			return a_rank < b_rank;
		if (a_rank >= 0)
			return true;
		if (b_rank >= 0)
			return false;
		return a.first.id < b.first.id;
	});
	std::vector<Sequential::ObjectToPrint> objects_out;
	for (const auto& o : objects) {
		objects_out.emplace_back(o.first);
		for (const auto& i : o.second)
			objects_out.emplace_back(i);
	}

	return objects_out;
}




void arrange_model_sequential(Model& model, const ConfigBase& config, bool current_bed_only)
{
	SeqArrange seq_arrange(model, config, current_bed_only);
	seq_arrange.process_seq_arrange([](int) {});
	seq_arrange.apply_seq_arrange(model);
}



SeqArrange::SeqArrange(const Model& model, const ConfigBase& config, bool current_bed_only)
{
	m_selected_bed = current_bed_only ? s_multiple_beds.get_active_bed() : -1;
	if (m_selected_bed != -1 && ! can_arrange_selected_bed(model, m_selected_bed))
		throw ExceptionCannotAttemptSeqArrange();

    m_printer_geometry = get_printer_geometry(config);
	m_solver_configuration = get_solver_config(m_printer_geometry);
	m_wipe_tower_relative_pos = optimal_sequential_wipe_tower_relative_pos(model, config);
	const SequentialClearanceScoring clearance_scoring = sequential_clearance_scoring(config);
	m_clearance_lift_radius = clearance_scoring.radius;
	m_clearance_lift_height = clearance_scoring.height;
	m_objects = get_objects_to_print(model, m_printer_geometry, m_selected_bed, m_wipe_tower_relative_pos, config);
	tune_solver_config_for_arrange(m_solver_configuration, m_objects.size());
	m_use_fixed_order_arrange = has_custom_sequential_order(model);
}



void SeqArrange::process_seq_arrange(std::function<void(int)> progress_fn)
{
	const SequentialClearanceScoring clearance_scoring{ m_clearance_lift_radius, m_clearance_lift_height };
	if (m_use_fixed_order_arrange) {
		m_plates = greedy_schedule_best_effort(m_solver_configuration, m_printer_geometry, m_objects, clearance_scoring, false);
		progress_fn(100);
	} else {
		m_plates =
			Sequential::schedule_ObjectsForSequentialPrint(
				m_solver_configuration,
				m_printer_geometry,
				m_objects, progress_fn);
		std::vector<Sequential::ScheduledPlate> greedy_plates;
		if (sequential_schedule_has_conflict(m_solver_configuration, m_printer_geometry, m_objects, m_plates)) {
			greedy_plates = greedy_schedule_best_effort(m_solver_configuration, m_printer_geometry, m_objects, clearance_scoring, true);
			m_plates = std::move(greedy_plates);
		} else {
			greedy_plates = greedy_schedule_best_effort(m_solver_configuration, m_printer_geometry, m_objects, clearance_scoring, true);
			if (is_better_schedule(greedy_plates, m_plates, m_objects, clearance_scoring) &&
				!sequential_schedule_has_conflict(m_solver_configuration, m_printer_geometry, m_objects, greedy_plates))
				m_plates = std::move(greedy_plates);
		}
	}

	// If this was arrangement of a single bed, check that all instances of a single object
	// ended up on the same bed. Otherwise we cannot apply the result (instances of a single
	// object always follow one another in the object list and therefore the print).
	if (m_selected_bed != -1 && s_multiple_beds.get_number_of_beds() > 1) {
		int expected_plate = -1;
		for (const Sequential::ObjectToPrint& otp : m_objects) {
			auto it = std::find_if(m_plates.begin(), m_plates.end(), [&otp](const auto& plate)
				{ return std::any_of(plate.scheduled_objects.begin(), plate.scheduled_objects.end(),
					[&otp](const auto& obj) { return otp.id == obj.id;
				});
			});
			assert(it != m_plates.end());
			size_t plate_id = it - m_plates.begin();
			if (expected_plate != -1 && size_t(expected_plate) != plate_id)
				throw ExceptionCannotApplySeqArrange();
			expected_plate = otp.glued_to_next ? int(plate_id) : -1;
		}
	}
}


// Extract the result and move the objects in Model accordingly.
void SeqArrange::apply_seq_arrange(Model& model) const
{
	struct MoveData {
		Sequential::ScheduledObject scheduled_object;
		size_t bed_idx;
		ModelObject* mo;
		ModelInstance* mi;
	};

	// Iterate over the result and move the instances.
	std::vector<MoveData> move_data_all; // Needed for the ordering.
	size_t plate_idx = 0;
	size_t new_number_of_beds = s_multiple_beds.get_number_of_beds();
	std::vector<int> touched_beds;
	for (const Sequential::ScheduledPlate& plate : m_plates) {
		int real_bed = plate_idx;
		if (m_selected_bed != -1) {
			// Only a single bed was arranged. Move "first" bed to its position
			// and everything else to newly created beds.
			real_bed += (plate_idx == 0 ? m_selected_bed : s_multiple_beds.get_number_of_beds() - 1);
		}
		touched_beds.emplace_back(real_bed);
		new_number_of_beds = std::max(new_number_of_beds, size_t(real_bed + 1));
		const Vec3d bed_offset = s_multiple_beds.get_bed_translation(real_bed);

		for (const Sequential::ScheduledObject& object : plate.scheduled_objects)
			for (ModelObject* mo : model.objects)
				for (ModelInstance* mi : mo->instances)
					if (int(mi->id().id) == object.id) {
						move_data_all.push_back({ object, size_t(real_bed), mo, mi });
						mi->set_offset(Vec3d(unscaled(object.x) + bed_offset.x(), unscaled(object.y) + bed_offset.y(), mi->get_offset().z()));
					}
		++plate_idx;
	}

	if (m_wipe_tower_relative_pos && !move_data_all.empty()) {
		const Vec2crd first_instance_pos = scaled(to_2d(move_data_all.front().mi->get_offset()));
		const Vec2crd wipe_tower_pos = first_instance_pos + *m_wipe_tower_relative_pos;
		model.wipe_tower().position = unscale(wipe_tower_pos);
	}

	// Create a copy of ModelObject pointers, zero ones present in move_data_all.
	// The point is to only reorder ModelObject which had actually been passed to the arrange algorithm.
	std::vector<ModelObject*> objects_reordered = model.objects;
	for (size_t i = 0; i < objects_reordered.size(); ++i) {
		ModelObject* mo = objects_reordered[i];
		if (std::any_of(move_data_all.begin(), move_data_all.end(), [&mo](const MoveData& md) { return md.mo == mo; }))
			objects_reordered[i] = nullptr;
	}
	// Fill the gaps with the arranged objects in the correct order.
	for (size_t i = 0; i < objects_reordered.size(); ++i) {
		if (! objects_reordered[i]) {
			objects_reordered[i] = move_data_all[0].mo;
			while (! move_data_all.empty() && move_data_all.front().mo == objects_reordered[i])
				move_data_all.erase(move_data_all.begin());
		}
	}

	// Check that the old and new vectors only differ in order of elements.
	auto a = model.objects;
	auto b = objects_reordered;
	std::sort(a.begin(), a.end());
	std::sort(b.begin(), b.end());
	if (a != b)
		std::terminate(); // A bug in the code above. Better crash now than later.

	// Update objects order in the model.
	std::swap(model.objects, objects_reordered);

	// One last thing. Move unprintable instances to new beds. It would be nicer to
	// arrange them (non-sequentially) on just one bed - maybe one day.
	std::map<int, std::vector<ModelInstance*>> instances_to_move; // bed to move from and list of instances
	for (ModelObject* mo : model.objects)
		for (ModelInstance* mi : mo->instances)
			if (!mi->printable) {
				auto it = s_multiple_beds.get_inst_map().find(mi->id());
				if (it == s_multiple_beds.get_inst_map().end() || (m_selected_bed != -1 && it->second != m_selected_bed))
					continue;
				// Was something placed on this bed during arrange? If not, we should not move anything.
				if (std::find(touched_beds.begin(), touched_beds.end(), it->second) != touched_beds.end())
					instances_to_move[it->second].emplace_back(mi);
			}
	// Now actually move them.
	for (auto& [bed_idx, instances] : instances_to_move) {
		Vec3d old_bed_offset = s_multiple_beds.get_bed_translation(bed_idx);
		Vec3d new_bed_offset = s_multiple_beds.get_bed_translation(new_number_of_beds);
		for (ModelInstance* mi : instances)
			mi->set_offset(mi->get_offset() - old_bed_offset + new_bed_offset);
		++new_number_of_beds;
	}
}



std::optional<SequentialCollisionInfo> check_seq_conflict(const Model& model, const ConfigBase& config)
{
	Sequential::PrinterGeometry printer_geometry = get_printer_geometry(config);
	Sequential::SolverConfiguration solver_config = get_solver_config(printer_geometry, Sequential::SEQ_DECIMATION_PRECISION_HIGH);
	std::vector<Sequential::ObjectToPrint> objects = get_objects_to_print(model, printer_geometry, -1, std::nullopt, config);

	if (printer_geometry.extruder_slices.empty()) {
		// If there are no data for extruder (such as extruder_clearance_radius set to 0),
		// consider it printable.
	        return {};
	}

	Sequential::ScheduledPlate plate;
	std::map<int, const ModelInstance*> objects_to_schedule;
	for (const ModelObject* mo : model.objects) {
		for (const ModelInstance* mi : mo->instances) {
			auto it = s_multiple_beds.get_inst_map().find(mi->id());
			if (it == s_multiple_beds.get_inst_map().end() || it->second != s_multiple_beds.get_active_bed())
				continue;

			// Is this instance in objects to print? It may be unprintable or something.
			auto it2 = std::find_if(objects.begin(), objects.end(), [&mi](const Sequential::ObjectToPrint& otp) { return otp.id == int(mi->id().id); });
			if (it2 == objects.end())
				continue;

			objects_to_schedule.emplace(int(mi->id().id), mi);
		}
	}
	for (const Sequential::ObjectToPrint& object : objects) {
		auto it = objects_to_schedule.find(object.id);
		if (it == objects_to_schedule.end())
			continue;
		const ModelInstance* mi = it->second;
		Vec3d offset = s_multiple_beds.get_bed_translation(s_multiple_beds.get_active_bed());
		plate.scheduled_objects.emplace_back(mi->id().id, scaled(mi->get_offset().x() - offset.x()), scaled(mi->get_offset().y() - offset.y()));
	}

	std::optional<Sequential::SequentialConflict> conflict = Sequential::check_ScheduledObjectsForSequentialConflictDetailed(solver_config, printer_geometry, objects, std::vector<Sequential::ScheduledPlate>(1, plate));
	if (conflict) {
		if (config.has("extruder_clearance_radius")) {
			const double clearance_radius = config.opt_float("extruder_clearance_radius");
			if (clearance_radius > 0.5) {
				DynamicPrintConfig relaxed_config(config);
				relaxed_config.set("extruder_clearance_radius", std::max(0.0, clearance_radius - 0.5));
				Sequential::PrinterGeometry relaxed_printer_geometry = get_printer_geometry(relaxed_config);
				Sequential::SolverConfiguration relaxed_solver_config = get_solver_config(relaxed_printer_geometry, Sequential::SEQ_DECIMATION_PRECISION_HIGH);
				std::vector<Sequential::ObjectToPrint> relaxed_objects = get_objects_to_print(model, relaxed_printer_geometry, -1, std::nullopt, relaxed_config);

				Sequential::ScheduledPlate relaxed_plate;
				for (const Sequential::ObjectToPrint& object : relaxed_objects) {
					auto it = objects_to_schedule.find(object.id);
					if (it == objects_to_schedule.end())
						continue;
					const ModelInstance* mi = it->second;
					Vec3d offset = s_multiple_beds.get_bed_translation(s_multiple_beds.get_active_bed());
					relaxed_plate.scheduled_objects.emplace_back(mi->id().id, scaled(mi->get_offset().x() - offset.x()), scaled(mi->get_offset().y() - offset.y()));
				}

				if (!Sequential::check_ScheduledObjectsForSequentialConflict(
						relaxed_solver_config, relaxed_printer_geometry, relaxed_objects, std::vector<Sequential::ScheduledPlate>(1, relaxed_plate)))
					return std::nullopt;
			}
		}

		return make_sequential_collision_info(*conflict, model);
	}
	return std::nullopt;
}

std::optional<SequentialCollisionInfo> check_seq_conflict(const Print& print, const ConfigBase& config)
{
	Sequential::PrinterGeometry printer_geometry = get_printer_geometry(config);
	Sequential::SolverConfiguration solver_config = get_solver_config(printer_geometry, Sequential::SEQ_DECIMATION_PRECISION_HIGH);
	std::vector<Sequential::ObjectToPrint> objects = get_objects_to_print(print.model(), printer_geometry, -1, std::nullopt, config);

	if (printer_geometry.extruder_slices.empty()) {
		// If there are no data for extruder (such as extruder_clearance_radius set to 0),
		// consider it printable.
		return {};
	}

	std::map<int, const PrintInstance*> objects_to_schedule;
	for (const PrintInstance* instance : sort_object_instances_by_model_order(print)) {
		if (instance == nullptr || instance->model_instance == nullptr)
			continue;

		auto it = std::find_if(objects.begin(), objects.end(), [instance](const Sequential::ObjectToPrint& otp) {
			return otp.id == int(instance->model_instance->id().id);
		});
		if (it == objects.end())
			continue;

		objects_to_schedule.emplace(int(instance->model_instance->id().id), instance);
	}

	Sequential::ScheduledPlate plate;
	for (const PrintInstance* instance : sort_object_instances_by_model_order(print)) {
		if (instance == nullptr || instance->model_instance == nullptr)
			continue;

		auto it = objects_to_schedule.find(int(instance->model_instance->id().id));
		if (it == objects_to_schedule.end())
			continue;

		plate.scheduled_objects.emplace_back(instance->model_instance->id().id, instance->shift.x(), instance->shift.y());
	}

	std::optional<Sequential::SequentialConflict> conflict = Sequential::check_ScheduledObjectsForSequentialConflictDetailed(solver_config, printer_geometry, objects, std::vector<Sequential::ScheduledPlate>(1, plate));
	if (conflict) {
		if (config.has("extruder_clearance_radius")) {
			const double clearance_radius = config.opt_float("extruder_clearance_radius");
			if (clearance_radius > 0.5) {
				DynamicPrintConfig relaxed_config(config);
				relaxed_config.set("extruder_clearance_radius", std::max(0.0, clearance_radius - 0.5));
				Sequential::PrinterGeometry relaxed_printer_geometry = get_printer_geometry(relaxed_config);
				Sequential::SolverConfiguration relaxed_solver_config = get_solver_config(relaxed_printer_geometry, Sequential::SEQ_DECIMATION_PRECISION_HIGH);
				std::vector<Sequential::ObjectToPrint> relaxed_objects = get_objects_to_print(print.model(), relaxed_printer_geometry, -1, std::nullopt, relaxed_config);

				Sequential::ScheduledPlate relaxed_plate;
				for (const PrintInstance* instance : sort_object_instances_by_model_order(print)) {
					if (instance == nullptr || instance->model_instance == nullptr)
						continue;

					auto it = std::find_if(relaxed_objects.begin(), relaxed_objects.end(), [instance](const Sequential::ObjectToPrint& otp) {
						return otp.id == int(instance->model_instance->id().id);
					});
					if (it != relaxed_objects.end())
						relaxed_plate.scheduled_objects.emplace_back(instance->model_instance->id().id, instance->shift.x(), instance->shift.y());
				}

				if (!Sequential::check_ScheduledObjectsForSequentialConflict(
						relaxed_solver_config, relaxed_printer_geometry, relaxed_objects, std::vector<Sequential::ScheduledPlate>(1, relaxed_plate)))
					return std::nullopt;
			}
		}

		return make_sequential_collision_info(*conflict, print.model());
	}

	return std::nullopt;
}


} // namespace Slic3r
