#include "libslic3r.h"
#include "Exception.hpp"
#include "Geometry.hpp"
#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"
#include "Line.hpp"
#include "clipper.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <list>
#include <map>
#include <numeric>
#include <set>
#include <utility>
#include <stack>
#include <vector>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/log/trivial.hpp>

#if defined(_MSC_VER) && defined(__clang__)
#define BOOST_NO_CXX17_HDR_STRING_VIEW
#endif

#include <boost/multiprecision/integer.hpp>

namespace Slic3r { namespace Geometry {

// This implementation is based on Andrew's monotone chain 2D convex hull algorithm
Polygon convex_hull(Points pts)
{
    std::sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });
    pts.erase(std::unique(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a.x() == b.x() && a.y() == b.y(); }), pts.end());

    Polygon hull;
    int n = (int)pts.size();
    if (n >= 3) {
        int k = 0;
        hull.points.resize(2 * n);
        // Build lower hull
        for (int i = 0; i < n; ++ i) {
            while (k >= 2 && pts[i].ccw(hull[k-2], hull[k-1]) <= 0)
                -- k;
            hull[k ++] = pts[i];
        }
        // Build upper hull
        for (int i = n-2, t = k+1; i >= 0; i--) {
            while (k >= t && pts[i].ccw(hull[k-2], hull[k-1]) <= 0)
                -- k;
            hull[k ++] = pts[i];
        }
        hull.points.resize(k);
        assert(hull.points.front() == hull.points.back());
        hull.points.pop_back();
    }
    return hull;
}

Pointf3s convex_hull(Pointf3s points)
{
    assert(points.size() >= 3);
    // sort input points
    std::sort(points.begin(), points.end(), [](const Vec3d &a, const Vec3d &b){ return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });

    int n = points.size(), k = 0;
    Pointf3s hull;

    if (n >= 3)
    {
        hull.resize(2 * n);

        // Build lower hull
        for (int i = 0; i < n; ++i)
        {
            Point p = Point::new_scale(points[i](0), points[i](1));
            while (k >= 2)
            {
                Point k1 = Point::new_scale(hull[k - 1](0), hull[k - 1](1));
                Point k2 = Point::new_scale(hull[k - 2](0), hull[k - 2](1));

                if (p.ccw(k2, k1) <= 0)
                    --k;
                else
                    break;
            }

            hull[k++] = points[i];
        }

        // Build upper hull
        for (int i = n - 2, t = k + 1; i >= 0; --i)
        {
            Point p = Point::new_scale(points[i](0), points[i](1));
            while (k >= t)
            {
                Point k1 = Point::new_scale(hull[k - 1](0), hull[k - 1](1));
                Point k2 = Point::new_scale(hull[k - 2](0), hull[k - 2](1));

                if (p.ccw(k2, k1) <= 0)
                    --k;
                else
                    break;
            }

            hull[k++] = points[i];
        }

        hull.resize(k);

        assert(hull.front() == hull.back());
        hull.pop_back();
    }

    return hull;
}

Polygon convex_hull(const Polygons &polygons)
{
    Points pp;
    for (Polygons::const_iterator p = polygons.begin(); p != polygons.end(); ++p) {
        pp.insert(pp.end(), p->points.begin(), p->points.end());
    }
    return convex_hull(std::move(pp));
}

bool directions_parallel(double angle1, double angle2, double max_diff)
{
    double diff = fabs(angle1 - angle2);
    max_diff += EPSILON;
    return diff < max_diff || fabs(diff - PI) < max_diff;
}

#if ENABLE_OUT_OF_BED_DETECTION_IMPROVEMENTS
bool directions_perpendicular(double angle1, double angle2, double max_diff)
{
    double diff = fabs(angle1 - angle2);
    max_diff += EPSILON;
    return fabs(diff - 0.5 * PI) < max_diff || fabs(diff - 1.5 * PI) < max_diff;
}
#endif // ENABLE_OUT_OF_BED_DETECTION_IMPROVEMENTS

template<class T>
bool contains(const std::vector<T> &vector, const Point &point)
{
    for (typename std::vector<T>::const_iterator it = vector.begin(); it != vector.end(); ++it) {
        if (it->contains(point)) return true;
    }
    return false;
}
template bool contains(const ExPolygons &vector, const Point &point);

double rad2deg_dir(double angle)
{
    angle = (angle < PI) ? (-angle + PI/2.0) : (angle + PI/2.0);
    if (angle < 0) angle += PI;
    return rad2deg(angle);
}

void simplify_polygons(const Polygons &polygons, double tolerance, Polygons* retval)
{
    Polygons pp;
    for (Polygons::const_iterator it = polygons.begin(); it != polygons.end(); ++it) {
        Polygon p = *it;
        p.points.push_back(p.points.front());
        p.points = MultiPoint::_douglas_peucker(p.points, tolerance);
        p.points.pop_back();
        pp.push_back(p);
    }
    *retval = Slic3r::simplify_polygons(pp);
}

double linint(double value, double oldmin, double oldmax, double newmin, double newmax)
{
    return (value - oldmin) * (newmax - newmin) / (oldmax - oldmin) + newmin;
}

#if 0
// Point with a weight, by which the points are sorted.
// If the points have the same weight, sort them lexicographically by their positions.
struct ArrangeItem {
    ArrangeItem() {}
    Vec2d    pos;
    coordf_t  weight;
    bool operator<(const ArrangeItem &other) const {
        return weight < other.weight ||
            ((weight == other.weight) && (pos(1) < other.pos(1) || (pos(1) == other.pos(1) && pos(0) < other.pos(0))));
    }
};

Pointfs arrange(size_t num_parts, const Vec2d &part_size, coordf_t gap, const BoundingBoxf* bed_bounding_box)
{
    // Use actual part size (the largest) plus separation distance (half on each side) in spacing algorithm.
    const Vec2d       cell_size(part_size(0) + gap, part_size(1) + gap);

    const BoundingBoxf bed_bbox = (bed_bounding_box != NULL && bed_bounding_box->defined) ? 
        *bed_bounding_box :
        // Bogus bed size, large enough not to trigger the unsufficient bed size error.
        BoundingBoxf(
            Vec2d(0, 0),
            Vec2d(cell_size(0) * num_parts, cell_size(1) * num_parts));

    // This is how many cells we have available into which to put parts.
    size_t cellw = size_t(floor((bed_bbox.size()(0) + gap) / cell_size(0)));
    size_t cellh = size_t(floor((bed_bbox.size()(1) + gap) / cell_size(1)));
    if (num_parts > cellw * cellh)
        throw Slic3r::InvalidArgument("%zu parts won't fit in your print area!\n", num_parts);
    
    // Get a bounding box of cellw x cellh cells, centered at the center of the bed.
    Vec2d       cells_size(cellw * cell_size(0) - gap, cellh * cell_size(1) - gap);
    Vec2d       cells_offset(bed_bbox.center() - 0.5 * cells_size);
    BoundingBoxf cells_bb(cells_offset, cells_size + cells_offset);
    
    // List of cells, sorted by distance from center.
    std::vector<ArrangeItem> cellsorder(cellw * cellh, ArrangeItem());
    for (size_t j = 0; j < cellh; ++ j) {
        // Center of the jth row on the bed.
        coordf_t cy = linint(j + 0.5, 0., double(cellh), cells_bb.min(1), cells_bb.max(1));
        // Offset from the bed center.
        coordf_t yd = cells_bb.center()(1) - cy;
        for (size_t i = 0; i < cellw; ++ i) {
            // Center of the ith column on the bed.
            coordf_t cx = linint(i + 0.5, 0., double(cellw), cells_bb.min(0), cells_bb.max(0));
            // Offset from the bed center.
            coordf_t xd = cells_bb.center()(0) - cx;
            // Cell with a distance from the bed center.
            ArrangeItem &ci = cellsorder[j * cellw + i];
            // Cell center
            ci.pos(0) = cx;
            ci.pos(1) = cy;
            // Square distance of the cell center to the bed center.
            ci.weight = xd * xd + yd * yd;
        }
    }
    // Sort the cells lexicographically by their distances to the bed center and left to right / bttom to top.
    std::sort(cellsorder.begin(), cellsorder.end());
    cellsorder.erase(cellsorder.begin() + num_parts, cellsorder.end());

    // Return the (left,top) corners of the cells.
    Pointfs positions;
    positions.reserve(num_parts);
    for (std::vector<ArrangeItem>::const_iterator it = cellsorder.begin(); it != cellsorder.end(); ++ it)
        positions.push_back(Vec2d(it->pos(0) - 0.5 * part_size(0), it->pos(1) - 0.5 * part_size(1)));
    return positions;
}
#else
class ArrangeItem {
public:
    Vec2d pos = Vec2d::Zero();
    size_t index_x, index_y;
    coordf_t dist;
};
class ArrangeItemIndex {
public:
    coordf_t index;
    ArrangeItem item;
    ArrangeItemIndex(coordf_t _index, ArrangeItem _item) : index(_index), item(_item) {};
};

bool
arrange(size_t total_parts, const Vec2d &part_size, coordf_t dist, const BoundingBoxf* bb, Pointfs &positions)
{
    positions.clear();

    Vec2d part = part_size;

    // use actual part size (the largest) plus separation distance (half on each side) in spacing algorithm
    part(0) += dist;
    part(1) += dist;
    
    Vec2d area(Vec2d::Zero());
    if (bb != NULL && bb->defined) {
        area = bb->size();
    } else {
        // bogus area size, large enough not to trigger the error below
        area(0) = part(0) * total_parts;
        area(1) = part(1) * total_parts;
    }
    
    // this is how many cells we have available into which to put parts
    size_t cellw = floor((area(0) + dist) / part(0));
    size_t cellh = floor((area(1) + dist) / part(1));
    if (total_parts > (cellw * cellh))
        return false;
    
    // total space used by cells
    Vec2d cells(cellw * part(0), cellh * part(1));
    
    // bounding box of total space used by cells
    BoundingBoxf cells_bb;
    cells_bb.merge(Vec2d(0,0)); // min
    cells_bb.merge(cells);  // max
    
    // center bounding box to area
    cells_bb.translate(
        (area(0) - cells(0)) / 2,
        (area(1) - cells(1)) / 2
    );
    
    // list of cells, sorted by distance from center
    std::vector<ArrangeItemIndex> cellsorder;
    
    // work out distance for all cells, sort into list
    for (size_t i = 0; i <= cellw-1; ++i) {
        for (size_t j = 0; j <= cellh-1; ++j) {
            coordf_t cx = linint(i + 0.5, 0, cellw, cells_bb.min(0), cells_bb.max(0));
            coordf_t cy = linint(j + 0.5, 0, cellh, cells_bb.min(1), cells_bb.max(1));
            
            coordf_t xd = fabs((area(0) / 2) - cx);
            coordf_t yd = fabs((area(1) / 2) - cy);
            
            ArrangeItem c;
            c.pos(0) = cx;
            c.pos(1) = cy;
            c.index_x = i;
            c.index_y = j;
            c.dist = xd * xd + yd * yd - fabs((cellw / 2) - (i + 0.5));
            
            // binary insertion sort
            {
                coordf_t index = c.dist;
                size_t low = 0;
                size_t high = cellsorder.size();
                while (low < high) {
                    size_t mid = (low + ((high - low) / 2)) | 0;
                    coordf_t midval = cellsorder[mid].index;
                    
                    if (midval < index) {
                        low = mid + 1;
                    } else if (midval > index) {
                        high = mid;
                    } else {
                        cellsorder.insert(cellsorder.begin() + mid, ArrangeItemIndex(index, c));
                        goto ENDSORT;
                    }
                }
                cellsorder.insert(cellsorder.begin() + low, ArrangeItemIndex(index, c));
            }
            ENDSORT: ;
        }
    }
    
    // the extents of cells actually used by objects
    coordf_t lx = 0;
    coordf_t ty = 0;
    coordf_t rx = 0;
    coordf_t by = 0;

    // now find cells actually used by objects, map out the extents so we can position correctly
    for (size_t i = 1; i <= total_parts; ++i) {
        ArrangeItemIndex c = cellsorder[i - 1];
        coordf_t cx = c.item.index_x;
        coordf_t cy = c.item.index_y;
        if (i == 1) {
            lx = rx = cx;
            ty = by = cy;
        } else {
            if (cx > rx) rx = cx;
            if (cx < lx) lx = cx;
            if (cy > by) by = cy;
            if (cy < ty) ty = cy;
        }
    }
    // now we actually place objects into cells, positioned such that the left and bottom borders are at 0
    for (size_t i = 1; i <= total_parts; ++i) {
        ArrangeItemIndex c = cellsorder.front();
        cellsorder.erase(cellsorder.begin());
        coordf_t cx = c.item.index_x - lx;
        coordf_t cy = c.item.index_y - ty;
        
        positions.push_back(Vec2d(cx * part(0), cy * part(1)));
    }
    
    if (bb != NULL && bb->defined) {
        for (Pointfs::iterator p = positions.begin(); p != positions.end(); ++p) {
            p->x() += bb->min(0);
            p->y() += bb->min(1);
        }
    }
    
    return true;
}
#endif

// Euclidian distance of two boost::polygon points.
template<typename T>
T dist(const boost::polygon::point_data<T> &p1,const boost::polygon::point_data<T> &p2)
{
	T dx = p2(0) - p1(0);
	T dy = p2(1) - p1(1);
	return sqrt(dx*dx+dy*dy);
}

// Find a foot point of "px" on a segment "seg".
template<typename segment_type, typename point_type>
inline point_type project_point_to_segment(segment_type &seg, point_type &px)
{
    typedef typename point_type::coordinate_type T;
    const point_type &p0 = low(seg);
    const point_type &p1 = high(seg);
    const point_type  dir(p1(0)-p0(0), p1(1)-p0(1));
    const point_type  dproj(px(0)-p0(0), px(1)-p0(1));
    const T           t = (dir(0)*dproj(0) + dir(1)*dproj(1)) / (dir(0)*dir(0) + dir(1)*dir(1));
    assert(t >= T(-1e-6) && t <= T(1. + 1e-6));
    return point_type(p0(0) + t*dir(0), p0(1) + t*dir(1));
}

void assemble_transform(Transform3d& transform, const Vec3d& translation, const Vec3d& rotation, const Vec3d& scale, const Vec3d& mirror)
{
    transform = Transform3d::Identity();
    transform.translate(translation);
    transform.rotate(Eigen::AngleAxisd(rotation(2), Vec3d::UnitZ()) * Eigen::AngleAxisd(rotation(1), Vec3d::UnitY()) * Eigen::AngleAxisd(rotation(0), Vec3d::UnitX()));
    transform.scale(scale.cwiseProduct(mirror));
}

Transform3d assemble_transform(const Vec3d& translation, const Vec3d& rotation, const Vec3d& scale, const Vec3d& mirror)
{
    Transform3d transform;
    assemble_transform(transform, translation, rotation, scale, mirror);
    return transform;
}

Vec3d extract_euler_angles(const Eigen::Matrix<double, 3, 3, Eigen::DontAlign>& rotation_matrix)
{
    // reference: http://www.gregslabaugh.net/publications/euler.pdf
    Vec3d angles1 = Vec3d::Zero();
    Vec3d angles2 = Vec3d::Zero();
    if (std::abs(std::abs(rotation_matrix(2, 0)) - 1.0) < 1e-5)
    {
        angles1(2) = 0.0;
        if (rotation_matrix(2, 0) < 0.0) // == -1.0
        {
            angles1(1) = 0.5 * (double)PI;
            angles1(0) = angles1(2) + ::atan2(rotation_matrix(0, 1), rotation_matrix(0, 2));
        }
        else // == 1.0
        {
            angles1(1) = - 0.5 * (double)PI;
            angles1(0) = - angles1(2) + ::atan2(- rotation_matrix(0, 1), - rotation_matrix(0, 2));
        }
        angles2 = angles1;
    }
    else
    {
        angles1(1) = -::asin(rotation_matrix(2, 0));
        double inv_cos1 = 1.0 / ::cos(angles1(1));
        angles1(0) = ::atan2(rotation_matrix(2, 1) * inv_cos1, rotation_matrix(2, 2) * inv_cos1);
        angles1(2) = ::atan2(rotation_matrix(1, 0) * inv_cos1, rotation_matrix(0, 0) * inv_cos1);

        angles2(1) = (double)PI - angles1(1);
        double inv_cos2 = 1.0 / ::cos(angles2(1));
        angles2(0) = ::atan2(rotation_matrix(2, 1) * inv_cos2, rotation_matrix(2, 2) * inv_cos2);
        angles2(2) = ::atan2(rotation_matrix(1, 0) * inv_cos2, rotation_matrix(0, 0) * inv_cos2);
    }

    // The following euristic is the best found up to now (in the sense that it works fine with the greatest number of edge use-cases)
    // but there are other use-cases were it does not
    // We need to improve it
    double min_1 = angles1.cwiseAbs().minCoeff();
    double min_2 = angles2.cwiseAbs().minCoeff();
    bool use_1 = (min_1 < min_2) || (is_approx(min_1, min_2) && (angles1.norm() <= angles2.norm()));

    return use_1 ? angles1 : angles2;
}

Vec3d extract_euler_angles(const Transform3d& transform)
{
    // use only the non-translational part of the transform
    Eigen::Matrix<double, 3, 3, Eigen::DontAlign> m = transform.matrix().block(0, 0, 3, 3);
    // remove scale
    m.col(0).normalize();
    m.col(1).normalize();
    m.col(2).normalize();
    return extract_euler_angles(m);
}

Transformation::Flags::Flags()
    : dont_translate(true)
    , dont_rotate(true)
    , dont_scale(true)
    , dont_mirror(true)
{
}

bool Transformation::Flags::needs_update(bool dont_translate, bool dont_rotate, bool dont_scale, bool dont_mirror) const
{
    return (this->dont_translate != dont_translate) || (this->dont_rotate != dont_rotate) || (this->dont_scale != dont_scale) || (this->dont_mirror != dont_mirror);
}

void Transformation::Flags::set(bool dont_translate, bool dont_rotate, bool dont_scale, bool dont_mirror)
{
    this->dont_translate = dont_translate;
    this->dont_rotate = dont_rotate;
    this->dont_scale = dont_scale;
    this->dont_mirror = dont_mirror;
}

Transformation::Transformation()
{
    reset();
}

Transformation::Transformation(const Transform3d& transform)
{
    set_from_transform(transform);
}

void Transformation::set_offset(const Vec3d& offset)
{
    set_offset(X, offset(0));
    set_offset(Y, offset(1));
    set_offset(Z, offset(2));
}

void Transformation::set_offset(Axis axis, double offset)
{
    if (m_offset(axis) != offset)
    {
        m_offset(axis) = offset;
        m_dirty = true;
    }
}

void Transformation::set_rotation(const Vec3d& rotation)
{
    set_rotation(X, rotation(0));
    set_rotation(Y, rotation(1));
    set_rotation(Z, rotation(2));
}

void Transformation::set_rotation(Axis axis, double rotation)
{
    rotation = angle_to_0_2PI(rotation);
    if (is_approx(std::abs(rotation), 2.0 * (double)PI))
        rotation = 0.0;

    if (m_rotation(axis) != rotation)
    {
        m_rotation(axis) = rotation;
        m_dirty = true;
    }
}

void Transformation::set_scaling_factor(const Vec3d& scaling_factor)
{
    set_scaling_factor(X, scaling_factor(0));
    set_scaling_factor(Y, scaling_factor(1));
    set_scaling_factor(Z, scaling_factor(2));
}

void Transformation::set_scaling_factor(Axis axis, double scaling_factor)
{
    if (m_scaling_factor(axis) != std::abs(scaling_factor))
    {
        m_scaling_factor(axis) = std::abs(scaling_factor);
        m_dirty = true;
    }
}

void Transformation::set_mirror(const Vec3d& mirror)
{
    set_mirror(X, mirror(0));
    set_mirror(Y, mirror(1));
    set_mirror(Z, mirror(2));
}

void Transformation::set_mirror(Axis axis, double mirror)
{
    double abs_mirror = std::abs(mirror);
    if (abs_mirror == 0.0)
        mirror = 1.0;
    else if (abs_mirror != 1.0)
        mirror /= abs_mirror;

    if (m_mirror(axis) != mirror)
    {
        m_mirror(axis) = mirror;
        m_dirty = true;
    }
}

void Transformation::set_from_transform(const Transform3d& transform)
{
    // offset
    set_offset(transform.matrix().block(0, 3, 3, 1));

    Eigen::Matrix<double, 3, 3, Eigen::DontAlign> m3x3 = transform.matrix().block(0, 0, 3, 3);

    // mirror
    // it is impossible to reconstruct the original mirroring factors from a matrix,
    // we can only detect if the matrix contains a left handed reference system
    // in which case we reorient it back to right handed by mirroring the x axis
    Vec3d mirror = Vec3d::Ones();
    if (m3x3.col(0).dot(m3x3.col(1).cross(m3x3.col(2))) < 0.0)
    {
        mirror(0) = -1.0;
        // remove mirror
        m3x3.col(0) *= -1.0;
    }
    set_mirror(mirror);

    // scale
    set_scaling_factor(Vec3d(m3x3.col(0).norm(), m3x3.col(1).norm(), m3x3.col(2).norm()));

    // remove scale
    m3x3.col(0).normalize();
    m3x3.col(1).normalize();
    m3x3.col(2).normalize();

    // rotation
    set_rotation(extract_euler_angles(m3x3));

    // forces matrix recalculation matrix
    m_matrix = get_matrix();

//    // debug check
//    if (!m_matrix.isApprox(transform))
//        std::cout << "something went wrong in extracting data from matrix" << std::endl;
}

void Transformation::reset()
{
    m_offset = Vec3d::Zero();
    m_rotation = Vec3d::Zero();
    m_scaling_factor = Vec3d::Ones();
    m_mirror = Vec3d::Ones();
    m_matrix = Transform3d::Identity();
    m_dirty = false;
}

const Transform3d& Transformation::get_matrix(bool dont_translate, bool dont_rotate, bool dont_scale, bool dont_mirror) const
{
    if (m_dirty || m_flags.needs_update(dont_translate, dont_rotate, dont_scale, dont_mirror))
    {
        m_matrix = Geometry::assemble_transform(
            dont_translate ? Vec3d::Zero() : m_offset, 
            dont_rotate ? Vec3d::Zero() : m_rotation,
            dont_scale ? Vec3d::Ones() : m_scaling_factor,
            dont_mirror ? Vec3d::Ones() : m_mirror
            );

        m_flags.set(dont_translate, dont_rotate, dont_scale, dont_mirror);
        m_dirty = false;
    }

    return m_matrix;
}

Transformation Transformation::operator * (const Transformation& other) const
{
    return Transformation(get_matrix() * other.get_matrix());
}

Transformation Transformation::volume_to_bed_transformation(const Transformation& instance_transformation, const BoundingBoxf3& bbox)
{
    Transformation out;

    if (instance_transformation.is_scaling_uniform()) {
        // No need to run the non-linear least squares fitting for uniform scaling.
        // Just set the inverse.
        out.set_from_transform(instance_transformation.get_matrix(true).inverse());
    }
    else if (is_rotation_ninety_degrees(instance_transformation.get_rotation()))
    {
        // Anisotropic scaling, rotation by multiples of ninety degrees.
        Eigen::Matrix3d instance_rotation_trafo =
            (Eigen::AngleAxisd(instance_transformation.get_rotation().z(), Vec3d::UnitZ()) *
            Eigen::AngleAxisd(instance_transformation.get_rotation().y(), Vec3d::UnitY()) *
            Eigen::AngleAxisd(instance_transformation.get_rotation().x(), Vec3d::UnitX())).toRotationMatrix();
        Eigen::Matrix3d volume_rotation_trafo =
            (Eigen::AngleAxisd(-instance_transformation.get_rotation().x(), Vec3d::UnitX()) *
            Eigen::AngleAxisd(-instance_transformation.get_rotation().y(), Vec3d::UnitY()) *
            Eigen::AngleAxisd(-instance_transformation.get_rotation().z(), Vec3d::UnitZ())).toRotationMatrix();

        // 8 corners of the bounding box.
        auto pts = Eigen::MatrixXd(8, 3);
        pts(0, 0) = bbox.min.x(); pts(0, 1) = bbox.min.y(); pts(0, 2) = bbox.min.z();
        pts(1, 0) = bbox.min.x(); pts(1, 1) = bbox.min.y(); pts(1, 2) = bbox.max.z();
        pts(2, 0) = bbox.min.x(); pts(2, 1) = bbox.max.y(); pts(2, 2) = bbox.min.z();
        pts(3, 0) = bbox.min.x(); pts(3, 1) = bbox.max.y(); pts(3, 2) = bbox.max.z();
        pts(4, 0) = bbox.max.x(); pts(4, 1) = bbox.min.y(); pts(4, 2) = bbox.min.z();
        pts(5, 0) = bbox.max.x(); pts(5, 1) = bbox.min.y(); pts(5, 2) = bbox.max.z();
        pts(6, 0) = bbox.max.x(); pts(6, 1) = bbox.max.y(); pts(6, 2) = bbox.min.z();
        pts(7, 0) = bbox.max.x(); pts(7, 1) = bbox.max.y(); pts(7, 2) = bbox.max.z();

        // Corners of the bounding box transformed into the modifier mesh coordinate space, with inverse rotation applied to the modifier.
        auto qs = pts *
            (instance_rotation_trafo *
            Eigen::Scaling(instance_transformation.get_scaling_factor().cwiseProduct(instance_transformation.get_mirror())) *
            volume_rotation_trafo).inverse().transpose();
        // Fill in scaling based on least squares fitting of the bounding box corners.
        Vec3d scale;
        for (int i = 0; i < 3; ++i)
            scale(i) = pts.col(i).dot(qs.col(i)) / pts.col(i).dot(pts.col(i));

        out.set_rotation(Geometry::extract_euler_angles(volume_rotation_trafo));
        out.set_scaling_factor(Vec3d(std::abs(scale(0)), std::abs(scale(1)), std::abs(scale(2))));
        out.set_mirror(Vec3d(scale(0) > 0 ? 1. : -1, scale(1) > 0 ? 1. : -1, scale(2) > 0 ? 1. : -1));
    }
    else
    {
        // General anisotropic scaling, general rotation.
        // Keep the modifier mesh in the instance coordinate system, so the modifier mesh will not be aligned with the world.
        // Scale it to get the required size.
        out.set_scaling_factor(instance_transformation.get_scaling_factor().cwiseInverse());
    }

    return out;
}

// For parsing a transformation matrix from 3MF / AMF.
Transform3d transform3d_from_string(const std::string& transform_str)
{
    assert(is_decimal_separator_point()); // for atof
    Transform3d transform = Transform3d::Identity();

    if (!transform_str.empty())
    {
        std::vector<std::string> mat_elements_str;
        boost::split(mat_elements_str, transform_str, boost::is_any_of(" "), boost::token_compress_on);

        unsigned int size = (unsigned int)mat_elements_str.size();
        if (size == 16)
        {
            unsigned int i = 0;
            for (unsigned int r = 0; r < 4; ++r)
            {
                for (unsigned int c = 0; c < 4; ++c)
                {
                    transform(r, c) = ::atof(mat_elements_str[i++].c_str());
                }
            }
        }
    }

    return transform;
}

Eigen::Quaterniond rotation_xyz_diff(const Vec3d &rot_xyz_from, const Vec3d &rot_xyz_to)
{
    return
        // From the current coordinate system to world.
        Eigen::AngleAxisd(rot_xyz_to(2), Vec3d::UnitZ()) * Eigen::AngleAxisd(rot_xyz_to(1), Vec3d::UnitY()) * Eigen::AngleAxisd(rot_xyz_to(0), Vec3d::UnitX()) *
        // From world to the initial coordinate system.
        Eigen::AngleAxisd(-rot_xyz_from(0), Vec3d::UnitX()) * Eigen::AngleAxisd(-rot_xyz_from(1), Vec3d::UnitY()) * Eigen::AngleAxisd(-rot_xyz_from(2), Vec3d::UnitZ());
}

// This should only be called if it is known, that the two rotations only differ in rotation around the Z axis.
double rotation_diff_z(const Vec3d &rot_xyz_from, const Vec3d &rot_xyz_to)
{
    Eigen::AngleAxisd angle_axis(rotation_xyz_diff(rot_xyz_from, rot_xyz_to));
    Vec3d  axis  = angle_axis.axis();
    double angle = angle_axis.angle();
#ifndef NDEBUG
    if (std::abs(angle) > 1e-8) {
        assert(std::abs(axis.x()) < 1e-8);
        assert(std::abs(axis.y()) < 1e-8);
    }
#endif /* NDEBUG */
    return (axis.z() < 0) ? -angle : angle;
}

namespace rotcalip {

using int256_t = boost::multiprecision::int256_t;
using int128_t = boost::multiprecision::int128_t;

template<class Scalar = int64_t>
inline Scalar magnsq(const Point &p)
{
    return Scalar(p.x()) * p.x() + Scalar(p.y()) * p.y();
}

template<class Scalar = int64_t>
inline Scalar dot(const Point &a, const Point &b)
{
    return Scalar(a.x()) * b.x() + Scalar(a.y()) * b.y();
}

template<class Scalar = int64_t>
inline Scalar dotperp(const Point &a, const Point &b)
{
    return Scalar(a.x()) * b.y() - Scalar(a.y()) * b.x();
}

using boost::multiprecision::abs;

// Compares the angle enclosed by vectors dir and dirA (alpha) with the angle
// enclosed by -dir and dirB (beta). Returns -1 if alpha is less than beta, 0
// if they are equal and 1 if alpha is greater than beta. Note that dir is
// reversed for beta, because it represents the opposite side of a caliper.
int cmp_angles(const Point &dir, const Point &dirA, const Point &dirB) {
    int128_t dotA = dot(dir, dirA);
    int128_t dotB = dot(-dir, dirB);
    int256_t dcosa = int256_t(magnsq(dirB)) * int256_t(abs(dotA)) * dotA;
    int256_t dcosb = int256_t(magnsq(dirA)) * int256_t(abs(dotB)) * dotB;
    int256_t diff = dcosa - dcosb;

    return diff > 0? -1 : (diff < 0 ? 1 : 0);
}

// A helper class to navigate on a polygon. Given a vertex index, one can
// get the edge belonging to that vertex, the coordinates of the vertex, the
// next and previous edges. Stuff that is needed in the rotating calipers algo.
class Idx
{
    size_t m_idx;
    const Polygon *m_poly;
public:
    explicit Idx(const Polygon &p): m_idx{0}, m_poly{&p} {}
    explicit Idx(size_t idx, const Polygon &p): m_idx{idx}, m_poly{&p} {}

    size_t idx() const { return m_idx; }
    void set_idx(size_t i) { m_idx = i; }
    size_t next() const { return (m_idx + 1) % m_poly->size(); }
    size_t inc() { return m_idx = (m_idx + 1) % m_poly->size(); }
    Point prev_dir() const {
        return pt() - (*m_poly)[(m_idx + m_poly->size() - 1) % m_poly->size()];
    }

    const Point &pt() const { return (*m_poly)[m_idx]; }
    const Point dir() const { return (*m_poly)[next()] - pt(); }
    const Point  next_dir() const
    {
        return (*m_poly)[(m_idx + 2) % m_poly->size()] - (*m_poly)[next()];
    }
    const Polygon &poly() const { return *m_poly; }
};

enum class AntipodalVisitMode { Full, EdgesOnly };

// Visit all antipodal pairs starting from the initial ia, ib pair which
// has to be a valid antipodal pair (not checked). fn is called for every
// antipodal pair encountered including the initial one.
// The callback Fn has a signiture of bool(size_t i, size_t j, const Point &dir)
// where i,j are the vertex indices of the antipodal pair and dir is the
// direction of the calipers touching the i vertex.
template<AntipodalVisitMode mode = AntipodalVisitMode::Full, class Fn>
void visit_antipodals (Idx& ia, Idx &ib, Fn &&fn)
{
    // Set current caliper direction to be the lower edge angle from X axis
    int cmp = cmp_angles(ia.prev_dir(), ia.dir(), ib.dir());
    Idx *current = cmp <= 0 ? &ia : &ib, *other = cmp <= 0 ? &ib : &ia;
    Idx *initial = current;
    bool visitor_continue = true;

    size_t start = initial->idx();
    bool finished = false;

    while (visitor_continue && !finished) {
        Point current_dir_a = current == &ia ? current->dir() : -current->dir();
        visitor_continue = fn(ia.idx(), ib.idx(), current_dir_a);

        // Parallel edges encountered. An additional pair of antipodals
        // can be yielded.
        if constexpr (mode == AntipodalVisitMode::Full)
            if (cmp == 0 && visitor_continue) {
                visitor_continue = fn(current == &ia ? ia.idx() : ia.next(),
                                      current == &ib ? ib.idx() : ib.next(),
                                      current_dir_a);
            }

        cmp = cmp_angles(current->dir(), current->next_dir(), other->dir());

        current->inc();
        if (cmp > 0) {
            std::swap(current, other);
        }

        if (initial->idx() == start) finished = true;
    }
}

} // namespace rotcalip

bool convex_polygons_intersect(const Polygon &A, const Polygon &B)
{
    using namespace rotcalip;

    // Establish starting antipodals as extremes in XY plane. Use the
    // easily obtainable bounding boxes to check if A and B is disjoint
    // and return false if the are.
    struct BB
    {
        size_t         xmin = 0, xmax = 0, ymin = 0, ymax = 0;
        const Polygon &P;
        static bool cmpy(const Point &l, const Point &u)
        {
            return l.y() < u.y() || (l.y() == u.y() && l.x() < u.x());
        }

        BB(const Polygon &poly): P{poly}
        {
            for (size_t i = 0; i < P.size(); ++i) {
                if (P[i] < P[xmin]) xmin = i;
                if (P[xmax] < P[i]) xmax = i;
                if (cmpy(P[i], P[ymin])) ymin = i;
                if (cmpy(P[ymax], P[i])) ymax = i;
            }
        }
    };

    BB bA{A}, bB{B};
    BoundingBox bbA{{A[bA.xmin].x(), A[bA.ymin].y()}, {A[bA.xmax].x(), A[bA.ymax].y()}};
    BoundingBox bbB{{B[bB.xmin].x(), B[bB.ymin].y()}, {B[bB.xmax].x(), B[bB.ymax].y()}};

//    if (!bbA.overlap(bbB))
//        return false;

    // Establish starting antipodals as extreme vertex pairs in X or Y direction
    // which reside on different polygons. If no such pair is found, the two
    // polygons are certainly not disjoint.
    Idx imin{bA.xmin, A}, imax{bB.xmax, B};
    if (B[bB.xmin] < imin.pt())  imin = Idx{bB.xmin, B};
    if (imax.pt()  < A[bA.xmax]) imax = Idx{bA.xmax, A};
    if (&imin.poly() == &imax.poly()) {
        imin = Idx{bA.ymin, A};
        imax = Idx{bB.ymax, B};
        if (B[bB.ymin] < imin.pt())  imin = Idx{bB.ymin, B};
        if (imax.pt()  < A[bA.ymax]) imax = Idx{bA.ymax, A};
    }

    if (&imin.poly() == &imax.poly())
        return true;

    bool found_divisor = false;
    visit_antipodals<AntipodalVisitMode::EdgesOnly>(
        imin, imax,
        [&imin, &imax, &found_divisor](size_t ia, size_t ib, const Point &dir) {
            //        std::cout << "A" << ia << " B" << ib << " dir " <<
            //        dir.x() << " " << dir.y() << std::endl;
            const Polygon &A = imin.poly(), &B = imax.poly();

            Point ref_a = A[(ia + 2) % A.size()], ref_b = B[(ib + 2) % B.size()];

            bool is_left_a = dotperp( dir, ref_a - A[ia]) > 0;
            bool is_left_b = dotperp(-dir, ref_b - B[ib]) > 0;

            // If both reference points are on the left (or right) of their
            // respective support lines and the opposite support line is to
            // the right (or left), the divisor line is found. We only test
            // the reference point, as by definition, if that is on one side,
            // all the other points must be on the same side of a support
            // line. If the support lines are collinear, the polygons must be
            // on the same side of their respective support lines.

            auto d = dotperp(dir, B[ib] - A[ia]);
            if (d == 0) {
                // The caliper lines are collinear, not just parallel
                found_divisor = (is_left_a && is_left_b) || (!is_left_a && !is_left_b);
            } else if (d > 0) { // B is to the left of (A, A+1)
                found_divisor = !is_left_a && !is_left_b;
            } else { // B is to the right of (A, A+1)
                found_divisor = is_left_a && is_left_b;
            }

            return !found_divisor;
        });

    // Intersects if the divisor was not found
    return !found_divisor;
}

}} // namespace Slic3r::Geometry
