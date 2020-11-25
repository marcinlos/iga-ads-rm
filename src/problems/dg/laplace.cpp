#include <vector>
#include <tuple>
#include <optional>
#include <cassert>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <type_traits>
#include <chrono>
#include <mutex>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/range/counting_range.hpp>

#include "ads/util.hpp"
#include "ads/util/iter/product.hpp"
#include "ads/bspline/bspline.hpp"
#include "ads/bspline/eval.hpp"
#include "ads/quad/gauss.hpp"
#include "ads/lin/tensor.hpp"
#include "ads/util/function_value.hpp"
#include "mumps.hpp"


namespace ads {

    template <typename T>
    auto as_signed(T a) {
        return std::make_signed_t<T>(a);
    }

    template <typename T>
    auto as_unsigned(T a) {
        return std::make_unsigned_t<T>(a);
    }


    using partition = std::vector<double>;

    using simple_index          = int;
    using simple_index_iterator = boost::counting_iterator<simple_index>;
    using simple_index_range    = boost::iterator_range<simple_index_iterator>;

    auto range(int start, int past_end) noexcept -> simple_index_range {
        return boost::counting_range(start, past_end);
    }

    auto empty_range() noexcept -> simple_index_range {
        return range(0, 0);
    }

    struct index_types {
        using index          = std::tuple<int, int>;
        using index_iterator = util::iter_product2<simple_index_iterator, index>;
        using index_range    = boost::iterator_range<index_iterator>;
    };


    struct interval {
        double left;
        double right;

        constexpr interval(double left, double right) noexcept
        : left{left}, right{right}
        { }
    };

    constexpr auto length(interval s) noexcept -> double {
        return std::abs(s.right - s.left);
    }

    auto subinterval(const partition& points, int i) noexcept -> interval {
        assert(i >= 0 && i < as_signed(points.size() - 1) && "Subinterval index out of range");
        const auto a = points[i];
        const auto b = points[i + 1];
        return {a, b};
    }

    constexpr auto lerp(double t, interval s) noexcept -> double {
        return (1 - t) * s.left + t * s.right;
    }

    auto operator <<(std::ostream& os, interval s) -> std::ostream& {
        return os << "[" << s.left << ", " << s.right << "]";
    }

    class interval_mesh {
    private:
        partition points_;

    public:
        using point            = double;
        using element_index    = simple_index;
        using element_iterator = simple_index_iterator;
        using element_range    = simple_index_range;
        using facet_index      = simple_index;
        using facet_range      = simple_index_range;

        explicit interval_mesh(partition points) noexcept
        : points_{std::move(points)}
        { }

        auto elements() const noexcept -> element_range {
            return range(0, element_count());
        }

        auto element_count() const noexcept -> int {
            return points_.size() - 1;
        }

        auto subinterval(element_index e) const noexcept -> interval {
            return ads::subinterval(points_, e);
        }

        struct point_data {
            point  position;
            double normal;
        };

        auto facets() const noexcept -> facet_range {
            const auto facet_count = points_.size();
            return range(0, facet_count);
        }

        auto boundary_facets() const noexcept -> std::array<facet_index, 2> {
            const auto last = points_.size() - 1;
            return {0, last};
        }

        auto facet(facet_index i) const noexcept -> point_data {
            assert(i < as_signed(points_.size()) && "Point index out of range");
            // all points are positive except for the first one
            const auto normal = i > 0 ? 1.0 : -1.0;
            return {points_[i], normal};
        }

    };

    enum class orientation {
        horizontal,
        vertical
    };

    class regular_mesh {
    private:
        interval_mesh mesh_x_;
        interval_mesh mesh_y_;

    public:
        using point            = std::tuple<double, double>;
        using element_index    = index_types::index;
        using element_iterator = index_types::index_iterator;
        using element_range    = index_types::index_range;

        struct edge_index {
            simple_index ix;
            simple_index iy;
            orientation  dir;
        };

        using facet_index      = edge_index;
        // using facet_iterator   = void;
        // using facet_range      = void;

        regular_mesh(partition xs, partition ys) noexcept
        : mesh_x_{std::move(xs)}
        , mesh_y_{std::move(ys)}
        { }

        auto elements() const noexcept -> element_range {
            const auto rx = mesh_x_.elements();
            const auto ry = mesh_y_.elements();
            return util::product_range<element_index>(rx, ry);
        }

        struct element_data {
            interval span_x;
            interval span_y;
        };

        struct edge_data {
            interval    span;
            double      position;
            orientation direction;
            point       normal;
        };

        auto element(element_index e) const noexcept -> element_data {
            const auto [ix, iy] = e;
            const auto sx = mesh_x_.subinterval(ix);
            const auto sy = mesh_y_.subinterval(iy);
            return {sx, sy};
        }

        auto facets() const noexcept -> std::vector<edge_index> {
            auto indices = std::vector<edge_index>{};

            for (auto ix : mesh_x_.elements()) {
                for (auto iy : mesh_y_.facets()) {
                    indices.push_back({ix, iy, orientation::horizontal});
                }
            }
            for (auto ix : mesh_x_.facets()) {
                for (auto iy : mesh_y_.elements()) {
                    indices.push_back({ix, iy, orientation::vertical});
                }
            }
            return indices;
        }

        auto boundary_facets() const noexcept -> std::vector<edge_index> {
            auto indices = std::vector<edge_index>{};

            for (auto iy : mesh_y_.boundary_facets()) {
                for (auto ix : mesh_x_.elements()) {
                    indices.push_back({ix, iy, orientation::horizontal});
                }
            }
            for (auto ix : mesh_x_.boundary_facets()) {
                for (auto iy : mesh_y_.elements()) {
                    indices.push_back({ix, iy, orientation::vertical});
                }
            }
            return indices;
        }

        auto facet(edge_index e) const noexcept -> edge_data {
            const auto [ix, iy, dir] = e;

            if (dir == orientation::horizontal) {
                const auto [y, ny] = mesh_y_.facet(iy);
                const auto span    = mesh_x_.subinterval(ix);
                const auto normal  = point{0, ny};
                return {span, y, dir, normal};
            } else {
                assert(dir == orientation::vertical && "Invalid edge orientation");
                const auto [x, nx] = mesh_x_.facet(ix);
                const auto span    = mesh_y_.subinterval(iy);
                const auto normal  = point{nx, 0};
                return {span, x, dir, normal};
            }
        }

    };

    auto operator <<(std::ostream& os, const regular_mesh::edge_index& edge) -> std::ostream& {
        const auto [ix, iy, dir] = edge;
        const char* sign = dir == orientation::horizontal ? "-" : "|";
        return os << "(" << ix << ", " << iy << ")[" << sign << "]";
    }


    using local_dof  = simple_index;
    using global_dof = simple_index;


    auto spans_for_elements(const bspline::basis& b) -> std::vector<int> {
        auto spans = std::vector<int>{};
        spans.reserve(b.elements());

        for (int i = 0; i + 1 < as_signed(b.knot_size()); ++ i) {
            if (b.knot[i] != b.knot[i + 1]) {
                spans.push_back(i);
            }
        }
        assert(as_signed(spans.size()) == b.elements());
        return spans;
    }


    class bspline_space {
    private:
        bspline::basis   basis_;
        std::vector<int> first_dofs_;
        std::vector<int> spans_;

    public:
        using point         = double;
        using element_index = simple_index;
        using facet_index   = simple_index;
        using dof_index     = simple_index;
        using dof_iterator  = simple_index_iterator;
        using dof_range     = simple_index_range;

        explicit bspline_space(bspline::basis basis)
        : basis_{std::move(basis)}
        , first_dofs_{first_nonzero_dofs(basis_)}
        , spans_{spans_for_elements(basis_)}
        { }

        auto basis() const noexcept -> const bspline::basis& {
            return basis_;
        }

        auto degree() const noexcept -> int {
            return basis_.degree;
        }

        auto dofs_per_element() const noexcept -> int {
            return degree() + 1;
        }

        auto dof_count() const noexcept -> int {
            return basis_.dofs();
        }

        auto dof_count(element_index) const noexcept -> int {
            return dofs_per_element();
        }

        auto facet_dof_count(facet_index f) const noexcept -> int {
            auto dofs = dofs_on_facet(f);
            using std::begin;
            using std::end;
            return std::distance(begin(dofs), end(dofs));
        }

        auto dofs() const noexcept -> dof_range {
            return range(0, dof_count());
        }

        auto dofs(element_index e) const noexcept -> dof_range {
            const auto first = first_dofs_[e];
            return range(first, first + dofs_per_element());
        }

        auto local_index(dof_index dof, element_index e) const noexcept -> local_dof {
            const auto first = first_dofs_[e];
            return dof - first;
        }

        auto first_dof(element_index e) const noexcept -> global_dof {
            return first_dofs_[e];
        }

        auto last_dof(element_index e) const noexcept -> global_dof {
            return first_dof(e) + dofs_per_element() - 1;
        }

        auto dofs_on_facet(facet_index f) const noexcept -> dof_range {
            const auto last_element  = basis_.elements() - 1;
            const auto elem_left     = std::max(f - 1, 0);
            const auto elem_right    = std::min(f, last_element);
            const auto first         = first_dofs_[elem_left];
            const auto one_past_last = first_dofs_[elem_right] + dofs_per_element();
            return range(first, one_past_last);
        }

        auto facet_local_index(dof_index dof, facet_index f) const noexcept -> local_dof {
            const auto elem_left = std::max(f - 1, 0);
            const auto first     = first_dofs_[elem_left];
            return dof - first;
        }

        auto span(element_index e) const noexcept -> int {
            return spans_[e];
        }
    };


    class bspline_basis_values {
    private:
        std::vector<double>   buffer_;
        std::vector<double**> point_data_; // indexed by point
        std::vector<double*>  dof_data_;   // indexed by point and derivative

    public:

        bspline_basis_values(int points, int dofs, int ders)
        : buffer_(points * dofs * (ders + 1))
        , point_data_(points)
        , dof_data_(points * (ders + 1)) {

            for (int i = 0; i < points * (ders + 1); ++ i) {
                dof_data_[i] = &buffer_[i * dofs];
            }
            for (int i = 0; i < points; ++ i) {
                point_data_[i] = &dof_data_[i * (ders + 1)];
            }
        }

        auto operator ()(int point, local_dof i, int der) const -> double {
            return point_data_[point][der][i];
        }

        auto point_buffer(int point) noexcept -> double** {
            return point_data_[point];
        }
    };


    auto evaluate_basis(const std::vector<double>& points, const bspline_space& space, int ders) -> bspline_basis_values {
        const auto point_count = static_cast<int>(points.size());
        const auto dof_count   = space.dofs_per_element();

        auto values  = bspline_basis_values{point_count, dof_count, ders};
        auto context = bspline::eval_ctx{space.degree()};

        for (int q = 0; q < as_signed(points.size()); ++ q) {
            const auto x      = points[q];
            const auto buffer = values.point_buffer(q);
            const auto span   = find_span(x, space.basis());
            eval_basis_with_derivatives(span, x, space.basis(), buffer, ders, context);
        }
        return values;
    }

    class bspline_basis_values_on_vertex {
    private:
        std::optional<bspline_basis_values> left_;
        std::optional<bspline_basis_values> right_;
        local_dof left_last_;
        local_dof right_first_;

    public:
        bspline_basis_values_on_vertex(std::optional<bspline_basis_values> left,
                                       std::optional<bspline_basis_values> right,
                                       local_dof left_last, local_dof right_first) noexcept
        : left_{std::move(left)}
        , right_{std::move(right)}
        , left_last_{left_last}
        , right_first_{right_first} {
            assert((left_ || right_) && "Neither left nor right adjacent element data specified");
        }

        auto operator ()(local_dof i, int der) const noexcept -> double {
            if (left_ && i <= left_last_) {
                return left(i, der);
            } else { // right_ has value
                return right(i, der);
            }
        }

        auto left(local_dof i, int der) const noexcept -> double {
            if (left_ && i <= left_last_) {
                return (*left_)(0, i, der);
            } else {
                return 0.0;
            }
        }

        auto right(local_dof i, int der) const noexcept -> double {
            if (right_ && i >= right_first_) {
                const auto idx = i - right_first_;
                return (*right_)(0, idx, der);
            } else {
                return 0.0;
            }
        }

        auto jump(local_dof i, int der, double normal) const noexcept -> double {
            const auto left_val  = left(i, der);
            const auto right_val = right(i, der);
            return normal * (left_val - right_val);
        }

        auto average(local_dof i, int der) const noexcept -> double {
            const auto left_val  = left(i, der);
            const auto right_val = right(i, der);
            const auto sum       = left_val + right_val;
            if (left_ && right_) {
                return sum / 2;
            } else {
                // one of these is zero
                return sum;
            }
        }
    };


    auto evaluate_basis_at_point(double x, const bspline_space& space, int ders, int span) -> bspline_basis_values {
        const auto dof_count = space.dofs_per_element();

        auto values  = bspline_basis_values{1, dof_count, ders};
        auto context = bspline::eval_ctx{space.degree()};

        const auto buffer = values.point_buffer(0);

        eval_basis_with_derivatives(span, x, space.basis(), buffer, ders, context);

        return values;
    }

    auto element_left(bspline_space::facet_index f, const bspline::basis&) -> std::optional<int> {
        return f > 0 ? f - 1 : std::optional<int>{};
    }

    auto element_right(bspline_space::facet_index f, const bspline::basis& b) -> std::optional<int> {
        return f < b.elements() ? f : std::optional<int>{};
    }

    auto evaluate_basis(bspline_space::facet_index f, const bspline_space& space, int ders)
                        -> bspline_basis_values_on_vertex {
        const auto& basis = space.basis();
        const auto  x     = basis.points[f];

        const auto maybe_elem_left  = element_left(f, space.basis());
        const auto maybe_elem_right = element_right(f, space.basis());

        if (maybe_elem_left && maybe_elem_right) {
            const auto elem_left       = maybe_elem_left.value();
            const auto elem_right      = maybe_elem_right.value();
            const auto span_left       = space.span(elem_left);
            const auto span_right      = space.span(elem_right);
            const auto left_last       = space.last_dof(elem_left);
            const auto right_first     = space.first_dof(elem_right);
            const auto left_last_loc   = space.facet_local_index(left_last, f);
            const auto right_first_loc = space.facet_local_index(right_first, f);

            auto vals_left  = evaluate_basis_at_point(x, space, ders, span_left);
            auto vals_right = evaluate_basis_at_point(x, space, ders, span_right);

            return {std::move(vals_left), std::move(vals_right), left_last_loc, right_first_loc};

        } else if (maybe_elem_left) {
            const auto elem_left     = maybe_elem_left.value();
            const auto span_left     = space.span(elem_left);
            const auto left_last     = space.last_dof(elem_left);
            const auto left_last_loc = space.facet_local_index(left_last, f);

            auto vals_left = evaluate_basis_at_point(x, space, ders, span_left);

            return {std::move(vals_left), {}, left_last_loc, {}};

        } else { // maybe_elem_right
            assert(maybe_elem_right && "No elements adjacent to specified face");
            const auto elem_right      = maybe_elem_right.value();
            const auto span_right      = space.span(elem_right);
            const auto right_first     = space.first_dof(elem_right);
            const auto right_first_loc = space.facet_local_index(right_first, f);

            auto vals_right = evaluate_basis_at_point(x, space, ders, span_right);

            return {{}, std::move(vals_right), {}, right_first_loc};
        }
    }


    class interval_quadrature_points {
    private:
        std::vector<double> points_;
        const double*       weights_;
        double              scale_;

    public:
        using point          = double;
        using point_index    = simple_index;
        using point_iterator = simple_index_iterator;
        using point_range    = simple_index_range;

        interval_quadrature_points(std::vector<double> points, const double* weights, double scale)
        : points_{std::move(points)}
        , weights_{weights}
        , scale_{scale}
        { }

        auto points() const noexcept -> const std::vector<double>& {
            return points_;
        }

        auto indices() const noexcept -> point_range {
            return range(0, points_.size());
        }

        auto coords(point_index q) const noexcept -> point {
            assert(q >= 0 && q < as_signed(points_.size()) && "Quadrature point index out of bounds");
            return points_[q];
        }

        auto weight(point_index q) const noexcept -> double {
            assert(q >= 0 && q < as_signed(points_.size()) && "Quadrature point index out of bounds");
            return weights_[q] * scale_;
        }

        struct point_data {
            point  x;
            double weight;
        };

        auto data(point_index q) const noexcept -> point_data {
            const auto x = coords(q);
            const auto w = weight(q);
            return {x, w};
        }
    };


    class tensor_quadrature_points {
    private:
        interval_quadrature_points ptx_;
        interval_quadrature_points pty_;

    public:
        using point          = std::tuple<double, double>;
        using point_index    = index_types::index;
        using point_iterator = index_types::index_iterator;
        using point_range    = index_types::index_range;

        tensor_quadrature_points(interval_quadrature_points ptx,
                                 interval_quadrature_points pty) noexcept
        : ptx_{std::move(ptx)}
        , pty_{std::move(pty)}
        { }

        auto xs() const noexcept -> const std::vector<double>& {
            return ptx_.points();
        }

        auto ys() const noexcept -> const std::vector<double>& {
            return pty_.points();
        }

        auto indices() const noexcept -> point_range {
            const auto rx = ptx_.indices();
            const auto ry = pty_.indices();
            return util::product_range<point_index>(rx, ry);
        }

        auto coords(point_index q) const noexcept -> point {
            const auto [ix, iy] = q;
            const auto x = ptx_.coords(ix);
            const auto y = pty_.coords(iy);
            return {x, y};
        }

        auto weight(point_index q) const noexcept -> double {
            const auto [ix, iy] = q;
            const auto wx = ptx_.weight(ix);
            const auto wy = pty_.weight(iy);
            return wx * wy;
        }

        struct point_data {
            point  x;
            double weight;
        };

        auto data(point_index q) const noexcept -> point_data {
            const auto x = coords(q);
            const auto w = weight(q);
            return {x, w};
        }
    };

    class edge_quadrature_points {
    private:
        interval_quadrature_points points_;
        double                     position_;
        orientation                direction_;

    public:
        using point          = std::tuple<double, double>;
        using point_index    = interval_quadrature_points::point_index;
        using point_iterator = interval_quadrature_points::point_iterator;
        using point_range    = interval_quadrature_points::point_range;

        edge_quadrature_points(interval_quadrature_points points, double position,
                               orientation direction) noexcept
        : points_{std::move(points)}
        , position_{position}
        , direction_{direction}
        { }

        auto points() const noexcept -> const std::vector<double>& {
            return points_.points();
        }

        auto position() const noexcept -> double {
            return position_;
        }

        auto indices() const noexcept -> point_range {
            return points_.indices();
        }

        auto coords(point_index q) const noexcept -> point {
            const auto s = points_.coords(q);
            if (direction_ == orientation::horizontal) {
                return {s, position_};
            } else {
                return {position_, s};
            }
        }

        auto weight(point_index q) const noexcept -> double {
            return points_.weight(q);
        }

        struct point_data {
            point  x;
            double weight;
        };

        auto data(point_index q) const noexcept -> point_data {
            const auto x = coords(q);
            const auto w = weight(q);
            return {x, w};
        }
    };


    class quadrature {
    private:
        regular_mesh* mesh_;
        int point_count_;

    public:
        using point          = regular_mesh::point;
        using element_index  = regular_mesh::element_index;
        using facet_index    = regular_mesh::facet_index;
        using point_set      = tensor_quadrature_points;
        using edge_point_set = edge_quadrature_points;

        quadrature(regular_mesh* mesh, int point_count)
        : mesh_{mesh}
        , point_count_{point_count}
        { }

        auto coordinates(element_index e) const -> point_set {
            const auto element = mesh_->element(e);

            auto ptx = data_for_interval(element.span_x);
            auto pty = data_for_interval(element.span_y);

            return {std::move(ptx), std::move(pty)};
        }

        auto coordinates(facet_index f) const -> edge_point_set {
            const auto edge = mesh_->facet(f);
            auto pts = data_for_interval(edge.span);

            return {std::move(pts), edge.position, edge.direction};
        }

    private:
        auto data_for_interval(interval target) const -> interval_quadrature_points {
            const auto size    = length(target);
            const auto scale   = size / 2;        // Gauss quadrature is defined for [-1, 1]
            const auto weights = quad::gauss::Ws[point_count_];

            return {transform_points(target), weights, scale};
        }

        auto transform_points(interval target) const -> std::vector<double> {
            auto points = std::vector<double>(point_count_);

            for (int i = 0; i < point_count_; ++ i) {
                const auto t = quad::gauss::Xs[point_count_][i];  // [-1, 1]
                const auto s = (t + 1) / 2;                       // [ 0, 1]
                points[i] = lerp(s, target);
            }
            return points;
        }
    };


    using value_type = ads::function_value_2d;


    class space {
    private:
        regular_mesh* mesh_;
        bspline_space space_x_;
        bspline_space space_y_;

        class evaluator;
        class edge_evaluator;

    public:
        using point         = regular_mesh::point;
        using element_index = regular_mesh::element_index;
        using facet_index   = regular_mesh::facet_index;
        using dof_index     = index_types::index;
        using dof_iterator  = index_types::index_iterator;
        using dof_range     = index_types::index_range;

        space(regular_mesh* mesh, bspline::basis bx, bspline::basis by)
        : mesh_{mesh}
        , space_x_{std::move(bx)}
        , space_y_{std::move(by)}
        { }

        auto mesh() const noexcept -> const regular_mesh& {
            return *mesh_;
        }

        auto space_x() const noexcept -> const bspline_space& {
            return space_x_;
        }

        auto space_y() const noexcept -> const bspline_space& {
            return space_y_;
        }

        auto dof_count() const noexcept -> int {
            const auto nx = space_x_.dof_count();
            const auto ny = space_y_.dof_count();

            return nx * ny;
        }

        auto dof_count(element_index e) const noexcept -> int {
            const auto [ex, ey] = e;
            const auto nx = space_x_.dof_count(ex);
            const auto ny = space_y_.dof_count(ey);

            return nx * ny;
        }

        auto facet_dof_count(facet_index f) const noexcept -> int {
            const auto [fx, fy, dir] = f;

            if (dir == orientation::horizontal) {
                const auto ndofs_x = space_x_.dofs_per_element();
                const auto ndofs_y = space_y_.facet_dof_count(fy);
                return ndofs_x * ndofs_y;
            } else {
                assert(dir == orientation::vertical && "Invalid edge orientation");
                const auto ndofs_x = space_x_.facet_dof_count(fx);
                const auto ndofs_y = space_y_.dofs_per_element();
                return ndofs_x * ndofs_y;
            }
        }

        auto dofs() const noexcept -> dof_range {
            const auto dofs_x = space_x_.dofs();
            const auto dofs_y = space_y_.dofs();

            return util::product_range<dof_index>(dofs_x, dofs_y);
        }

        auto dofs(element_index e) const noexcept -> dof_range {
            const auto [ex, ey] = e;
            const auto dofs_x = space_x_.dofs(ex);
            const auto dofs_y = space_y_.dofs(ey);

            return util::product_range<dof_index>(dofs_x, dofs_y);
        }

        auto dofs_on_facet(facet_index f) const noexcept -> dof_range {
            const auto [ix, iy, dir] = f;

            if (dir == orientation::horizontal) {
                const auto dofs_x = space_x_.dofs(ix);
                const auto dofs_y = space_y_.dofs_on_facet(iy);
                return util::product_range<dof_index>(dofs_x, dofs_y);
            } else {
                assert(dir == orientation::vertical && "Invalid edge orientation");
                const auto dofs_x = space_x_.dofs_on_facet(ix);
                const auto dofs_y = space_y_.dofs(iy);
                return util::product_range<dof_index>(dofs_x, dofs_y);
            }
        }

        auto local_index(dof_index dof, element_index e) const noexcept -> local_dof {
            const auto idx = index_on_element(dof, e);

            const auto ndofs_x = space_x_.dofs_per_element();
            const auto ndofs_y = space_y_.dofs_per_element();

            return linearized(idx, {ndofs_x, ndofs_y});
        }

        auto facet_local_index(dof_index dof, facet_index f) const noexcept -> local_dof {
            const auto idx = index_on_facet(dof, f);

            const auto [fx, fy, dir] = f;

            if (dir == orientation::horizontal) {
                const auto ndofs_x = space_x_.dofs_per_element();
                const auto ndofs_y = space_y_.facet_dof_count(fy);
                return linearized(idx, {ndofs_x, ndofs_y});
            } else {
                assert(dir == orientation::vertical && "Invalid edge orientation");
                const auto ndofs_x = space_x_.facet_dof_count(fx);
                const auto ndofs_y = space_y_.dofs_per_element();
                return linearized(idx, {ndofs_x, ndofs_y});
            }
        }

        auto global_index(dof_index dof) const noexcept -> global_dof {
            const auto ndofs_x = space_x_.dof_count();
            const auto ndofs_y = space_y_.dof_count();

            return linearized(dof, {ndofs_x, ndofs_y});
        }


        auto dof_evaluator(element_index e, const tensor_quadrature_points& points, int ders) const -> evaluator {
            auto data_x = evaluate_basis(points.xs(), space_x_, ders);
            auto data_y = evaluate_basis(points.ys(), space_y_, ders);

            return evaluator{this, e, ders, std::move(data_x), std::move(data_y)};
        }

        auto dof_evaluator(facet_index f, const edge_quadrature_points& points, int ders) const -> edge_evaluator {
            const auto [fx, fy, dir] = f;
            if (dir == orientation::horizontal) {
                auto data_x = evaluate_basis(points.points(), space_x_, ders);
                auto data_y = evaluate_basis(fy, space_y_, ders);
                return edge_evaluator{this, f, ders, std::move(data_x), std::move(data_y)};
            } else {
                assert(dir == orientation::vertical && "Invalid edge orientation");
                auto data_x = evaluate_basis(fx, space_x_, ders);
                auto data_y = evaluate_basis(points.points(), space_y_, ders);
                return edge_evaluator{this, f, ders, std::move(data_y), std::move(data_x)};
            }
        }

    private:

        class evaluator {
        private:
            const space*         space_;
            element_index        element_;
            int                  derivatives_;
            bspline_basis_values vals_x_;
            bspline_basis_values vals_y_;

        public:
            using point_index = tensor_quadrature_points::point_index;

            evaluator(const space* space, element_index element, int derivatives,
                      bspline_basis_values vals_x, bspline_basis_values vals_y) noexcept
            : space_{space}
            , element_{element}
            , derivatives_{derivatives}
            , vals_x_{std::move(vals_x)}
            , vals_y_{std::move(vals_y)}
            { }

            auto operator ()(dof_index dof, point_index q) const noexcept -> value_type {
                const auto [qx, qy] = q;
                const auto [ix, iy] = space_->index_on_element(dof, element_);

                const auto  Bx = vals_x_(qx, ix, 0);
                const auto dBx = vals_x_(qx, ix, 1);
                const auto  By = vals_y_(qy, iy, 0);
                const auto dBy = vals_y_(qy, iy, 1);

                const auto v  =  Bx *  By;
                const auto dx = dBx *  By;
                const auto dy =  Bx * dBy;

                return {v, dx, dy};
            }
        };

        class edge_evaluator {
        private:
            const space*                   space_;
            facet_index                    facet_;
            int                            derivatives_;
            bspline_basis_values           vals_interval_;
            bspline_basis_values_on_vertex vals_point_;

        public:
            using point_index = edge_quadrature_points::point_index;

            edge_evaluator(const space* space, facet_index facet, int derivatives,
                           bspline_basis_values vals_interval,
                           bspline_basis_values_on_vertex vals_point) noexcept
            : space_{space}
            , facet_{facet}
            , derivatives_{derivatives}
            , vals_interval_{std::move(vals_interval)}
            , vals_point_{std::move(vals_point)} { }

            auto operator ()(dof_index dof, point_index q) const noexcept -> value_type {
                const auto [ix, iy] = space_->index_on_facet(dof, facet_);

                if (facet_.dir == orientation::horizontal) {
                    const auto  Bx = vals_interval_(q, ix, 0);
                    const auto dBx = vals_interval_(q, ix, 1);
                    const auto  By = vals_point_(iy, 0);
                    const auto dBy = vals_point_(iy, 1);

                    const auto v  =  Bx *  By;
                    const auto dx = dBx *  By;
                    const auto dy =  Bx * dBy;

                    return {v, dx, dy};
                } else {
                    const auto  Bx = vals_point_(ix, 0);
                    const auto dBx = vals_point_(ix, 1);
                    const auto  By = vals_interval_(q, iy, 0);
                    const auto dBy = vals_interval_(q, iy, 1);

                    const auto v  =  Bx *  By;
                    const auto dx = dBx *  By;
                    const auto dy =  Bx * dBy;

                    return {v, dx, dy};
                }
            }
        };

        auto index_on_element(dof_index dof, element_index e) const noexcept -> dof_index {
            const auto [ex, ey] = e;
            const auto [dx, dy] = dof;

            const auto ix = space_x_.local_index(dx, ex);
            const auto iy = space_y_.local_index(dy, ey);

            return {ix, iy};
        }

        auto index_on_facet(dof_index dof, facet_index f) const noexcept -> dof_index {
            const auto [fx, fy, dir] = f;
            const auto [dx, dy]      = dof;

            if (dir == orientation::horizontal) {
                const auto ix = space_x_.local_index(dx, fx);
                const auto iy = space_y_.facet_local_index(dy, fy);
                return {ix, iy};
            } else {
                assert(dir == orientation::vertical && "Invalid edge orientation");
                const auto ix = space_x_.facet_local_index(dx, fx);
                const auto iy = space_y_.local_index(dy, fy);
                return {ix, iy};
            }
        }

        auto linearized(dof_index dof, std::array<int, 2> bounds) const noexcept -> simple_index {
            const auto [ix, iy] = dof;
            const auto [nx, ny] = bounds;
            const auto order = standard_ordering<2>{{nx, ny}};
            return order.linear_index(ix, iy);
        }
    };


    class bspline_function {
    private:
        const space*              space_;
        std::vector<double>       coefficients_;
        mutable bspline::eval_ctx ctx_x_;
        mutable bspline::eval_ctx ctx_y_;
        mutable std::mutex        ctx_lock_;

    public:
        using point  = space::point;

        explicit bspline_function(const space* space)
        : space_{space}
        , coefficients_(space->dof_count())
        , ctx_x_{space->space_x().degree()}
        , ctx_y_{space->space_y().degree()}
        { }

        auto operator ()(point p) const noexcept -> double {
            const auto [x, y] = p;

            auto coeffs = [this](int i, int j) {
                const auto idx = space_->global_index({i, j});
                return coefficients_[idx];
            };

            const auto& bx = space_->space_x().basis();
            const auto& by = space_->space_y().basis();

            std::scoped_lock guard{ctx_lock_};
            return bspline::eval(x, y, coeffs, bx, by, ctx_x_, ctx_y_);
        }

        auto data() noexcept -> double* {
            return coefficients_.data();
        }
    };


    auto evenly_spaced(double a, double b, int elems) -> partition {
        assert(elems > 0 && "Invalid number of partition elements");
        auto xs = partition(elems + 1);

        for (int i = 0; i <= elems; ++ i) {
            xs[i] = lerp(i, elems, a, b);
        }
        return xs;
    }

    auto make_bspline_basis(const partition& points, int p, int c) -> bspline::basis {
        const auto n = as_signed(points.size());
        const auto r = p - c;
        auto knot = bspline::knot_vector{};
        knot.reserve((p + 1) + (n - 1) * r + (p + 1));

        auto append = [&knot](int k, double x) {
            std::fill_n(back_inserter(knot), k, x);
        };

        append(p + 1, points[0]);
        for (int i = 1; i < n - 1; ++ i) {
            append(r, points[i]);
        }
        append(p + 1, points[n - 1]);

        return bspline::basis{std::move(knot), p};
    }
}

int main() {
    auto elems = 128;
    auto p     = 3;
    auto c     = 1;
    auto h     = 1.0 / elems;
    auto eta   = 1000000.0 / h;

    auto xs = ads::evenly_spaced(0.0, 1.0, elems);
    auto ys = ads::evenly_spaced(0.0, 1.0, elems);

    auto bx = ads::make_bspline_basis(xs, p, c);
    auto by = ads::make_bspline_basis(ys, p, c);


    // auto spans = ads::spans_for_elements(bx);
    // for (int i = 0; i < ads::as_signed(spans.size()); ++ i) {
    //     std::cout << "element " << i << "  -> span " << spans[i] << std::endl;
    // }

    auto mesh = ads::regular_mesh{xs, ys};
    auto space = ads::space{&mesh, bx, by};
    auto quad = ads::quadrature{&mesh, p + 1};

    auto n = space.dof_count();
    std::cout << "DoFs: " << n << std::endl;

    auto F = ads::bspline_function(&space);
    auto problem = mumps::problem{F.data(), n};
    auto solver  = mumps::solver{};

    auto diff = [](auto before, auto after) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(after - before).count();
    };

    // const auto& xspace = space.space_x();
    // auto xmesh = ads::interval_mesh{xs};

    // for (auto f : xmesh.facets()) {
    //     auto vals = evaluate_basis(f, xspace, 1);
    //     auto [x, normal] = xmesh.facet(f);
    //     std::cout << "Point " << x << ", n = " << normal << std::endl;
    //     for (auto dof : xspace.dofs_on_facet(f)) {
    //         auto i      = xspace.facet_local_index(dof, f);
    //         auto val    = vals(i, 0);
    //         auto left   = vals.left(i, 0);
    //         auto right  = vals.right(i, 0);
    //         auto avg    = vals.average(i, 0);
    //         auto jump   = vals.jump(i, 0, normal);
    //         auto dleft  = vals.left(i, 1);
    //         auto dright = vals.right(i, 1);
    //         auto dval   = vals(i, 1);
    //         auto davg   = vals.average(i, 1);
    //         auto djump  = vals.jump(i, 1, normal);
    //         std::cout << "  DOF " << dof << " (local: " << i << ")" <<std::endl;
    //         std::cout << "    val =   " << std::setw(5) << val << "    " << dval << std::endl;
    //         std::cout << "    left =  " << std::setw(5) << left << "    " << dleft << std::endl;
    //         std::cout << "    right = " << std::setw(5) << right << "    " << dright << std::endl;
    //         std::cout << "    avg =   " << std::setw(5) << avg << "    " << davg << std::endl;
    //         std::cout << "    jump =  " << std::setw(5) << jump << "    " << djump << std::endl;
    //     }
    // }

    // for (auto f : mesh.boundary_facets()) {
    // for (auto f : mesh.facets()) {
        // std::cout << "Facet " << f << " :  ";
        // auto facet = mesh.facet(f);
        // auto [x, y] = facet.normal;
        // if (facet.direction == ads::orientation::horizontal) {
        //     std::cout <<  facet.span << " x {" << facet.position << "}";
        // } else {
        //     std::cout << "{" << facet.position << "} x " << facet.span;
        // }
        // std::cout << ",  n = (" << x << ", " << y << ") ";
        // std::cout << ", dofs: " << space.facet_dof_count(f) << std::endl;
        // for (auto [ix, iy] : space.dofs_on_facet(f)) {
        //     std::cout << "   (" << ix << ", " << iy << ") -> " << space.facet_local_index({ix,iy}, f) <<  std::endl;
        // }
        // std::cout << std::endl;
        // auto points = quad.coordinates(f);
        // for (auto q : points.indices()) {
        //     auto [X, w] = points.data(q);
        //     auto [x, y] = X;
        //     std::cout << "  " << q << "  -> (" << x << ", " << y << "), w = " << w << std::endl;
        // }
    // }

    auto t_before_matrix = std::chrono::steady_clock::now();

    // auto matrix = std::unordered_map<ads::index_types::index, double>{};

    for (auto e : mesh.elements()) {
        auto points = quad.coordinates(e);
        auto eval   = space.dof_evaluator(e, points, 1);

        auto n = space.dof_count(e);
        auto M = ads::lin::tensor<double, 2>{{n, n}};

        for (auto q : points.indices()) {
            auto [x, w] = points.data(q);

            for (auto i : space.dofs(e)) {
                auto u = eval(i, q);
                for (auto j : space.dofs(e)) {
                    auto v = eval(j, q);

                    auto iloc = space.local_index(i, e);
                    auto jloc = space.local_index(j, e);

                    auto form = u.dx * v.dx + u.dy * v.dy;

                    M(jloc, iloc) += form * w;
                }
            }
        }

        for (auto i : space.dofs(e)) {
            auto iloc = space.local_index(i, e);
            auto I    = space.global_index(i);
            for (auto j : space.dofs(e)) {
                auto jloc = space.local_index(j, e);
                auto J    = space.global_index(j);

                problem.add(J + 1, I + 1, M(jloc, iloc));
            }
        }
    }
    auto t_after_matrix = std::chrono::steady_clock::now();

    auto t_before_boundary = std::chrono::steady_clock::now();

    for (auto f : mesh.boundary_facets()) {
        auto facet = mesh.facet(f);
        auto [nx, ny] = facet.normal;

        auto points = quad.coordinates(f);
        auto eval   = space.dof_evaluator(f, points, 1);

        auto n = space.facet_dof_count(f);
        auto M = ads::lin::tensor<double, 2>{{n, n}};

        for (auto q : points.indices()) {
            auto [x, w] = points.data(q);

            for (auto i : space.dofs_on_facet(f)) {
                auto u = eval(i, q);
                for (auto j : space.dofs_on_facet(f)) {
                    auto v = eval(j, q);

                    auto iloc = space.facet_local_index(i, f);
                    auto jloc = space.facet_local_index(j, f);

                    auto form = - (v.dx * nx + v.dy * ny) * u.val
                                - (u.dx * nx + u.dy * ny) * v.val
                                + eta * u.val * v.val;

                    M(jloc, iloc) += form * w;
                }
            }
        }

        for (auto i : space.dofs_on_facet(f)) {
            auto iloc = space.facet_local_index(i, f);
            auto I    = space.global_index(i);
            for (auto j : space.dofs_on_facet(f)) {
                auto jloc = space.facet_local_index(j, f);
                auto J    = space.global_index(j);

                // problem.add(J + 1, I + 1, M(jloc, iloc));
                auto val = M(jloc, iloc);
                if (val != 0.0) {
                    problem.add(J + 1, I + 1, val);
                }
            }
        }
    }

    auto t_after_boundary = std::chrono::steady_clock::now();

    std::cout << "Non-zeros: " << problem.nonzero_entries() << std::endl;
    std::cout << "Computing RHS" << std::endl;

#define SQ(x) ((x)*(x))
    auto func = []([[maybe_unused]] double x, [[maybe_unused]] double y) {
        // return 2 * M_PI * M_PI * std::sin(M_PI * x) * std::sin(M_PI * y);
        // return -2 * M_PI * M_PI * (
        //         std::cos(2 * M_PI * x) * SQ(std::sin(M_PI * y)) +
        //         std::cos(2 * M_PI * y) * SQ(std::sin(M_PI * x)));
        return 0.0;
    };

    auto g = []([[maybe_unused]] double x, [[maybe_unused]] double y) {
        // return 1.0;
        return x*x + y*y;
    };

    auto sol = [](double x, double y) {
        return 1 + std::sin(M_PI * x) * std::sin(M_PI * y);
        // return SQ(std::sin(M_PI * x) * std::sin(M_PI * y));
    };

    auto t_before_rhs = std::chrono::steady_clock::now();
    for (auto e : mesh.elements()) {
        auto points = quad.coordinates(e);
        auto eval   = space.dof_evaluator(e, points, 1);

        auto n = space.dof_count(e);
        auto M = ads::lin::tensor<double, 1>{{n}};

        for (auto q : points.indices()) {
            auto [X, w] = points.data(q);
            auto [x, y] = X;

            for (auto j : space.dofs(e)) {
                auto v    = eval(j, q);
                auto jloc = space.local_index(j, e);

                auto form = v.val * func(x, y);

                M(jloc) += form * w;
            }
        }
        for (auto j : space.dofs(e)) {
            auto jloc = space.local_index(j, e);
            auto J    = space.global_index(j);
            F.data()[J] += M(jloc);
        }
    }
    auto t_after_rhs = std::chrono::steady_clock::now();

    auto t_before_rhs_bnd = std::chrono::steady_clock::now();
    for (auto f : mesh.boundary_facets()) {
        auto facet = mesh.facet(f);
        auto [nx, ny] = facet.normal;

        auto points = quad.coordinates(f);
        auto eval   = space.dof_evaluator(f, points, 1);

        auto n = space.facet_dof_count(f);
        auto M = ads::lin::tensor<double, 1>{{n}};

        for (auto q : points.indices()) {
            auto [X, w] = points.data(q);
            auto [x, y] = X;
            auto gval = g(x, y);

            for (auto j : space.dofs_on_facet(f)) {
                auto v    = eval(j, q);
                auto jloc = space.facet_local_index(j, f);

                auto form = - (v.dx * nx + v.dy * ny) * gval
                            + eta * gval * v.val;

                M(jloc) += form * w;
            }
        }
        for (auto j : space.dofs_on_facet(f)) {
            auto jloc = space.facet_local_index(j, f);
            auto J    = space.global_index(j);
            F.data()[J] += M(jloc);
        }
    }
    auto t_after_rhs_bnd = std::chrono::steady_clock::now();

    std::cout << "Solving" << std::endl;
    auto t_before_solver = std::chrono::steady_clock::now();
    solver.solve(problem);
    auto t_after_solver = std::chrono::steady_clock::now();

    std::cout << "Computing error" << std::endl;

    auto t_before_err = std::chrono::steady_clock::now();

    auto err = 0.0;

    for (auto e : mesh.elements()) {
        auto points = quad.coordinates(e);
        for (auto q : points.indices()) {
            auto [X, w] = points.data(q);
            auto [x, y] = X;

            auto val   = F(X);
            auto exact = sol(x, y);
            auto d = val - exact;
            err += d * d * w;
        }
    }
    err = std::sqrt(err);

    auto t_after_err = std::chrono::steady_clock::now();

    auto t_before_output = std::chrono::steady_clock::now();
    {
        auto out = std::ofstream{"result.data"};
        for (auto x : ads::evenly_spaced(0.0, 1.0, 100)) {
            for (auto y : ads::evenly_spaced(0.0, 1.0, 100)) {
                auto val = F({x, y});
                out << x << " " << y << " " << val << std::endl;
            }
        }
    }
    auto t_after_output = std::chrono::steady_clock::now();

    std::cout << "error = " << err << std::endl;
    std::cout << "Matrix: " << std::setw(6) << diff(t_before_matrix, t_after_matrix)     << " ms" << std::endl;
    std::cout << "Bndry : " << std::setw(6) << diff(t_before_boundary, t_after_boundary) << " ms" << std::endl;
    std::cout << "RHS:    " << std::setw(6) << diff(t_before_rhs, t_after_rhs)           << " ms" << std::endl;
    std::cout << "RHS bd: " << std::setw(6) << diff(t_before_rhs_bnd, t_after_rhs_bnd)   << " ms" << std::endl;
    std::cout << "Solver: " << std::setw(6) << diff(t_before_solver, t_after_solver)     << " ms" << std::endl;
    std::cout << "Error:  " << std::setw(6) << diff(t_before_err, t_after_err)           << " ms" << std::endl;
    std::cout << "Output: " << std::setw(6) << diff(t_before_output, t_after_output)     << " ms" << std::endl;
}

