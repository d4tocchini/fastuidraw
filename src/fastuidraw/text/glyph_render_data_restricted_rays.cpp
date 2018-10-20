/*!
 * \file glyph_render_data_restricted_rays.cpp
 * \brief file glyph_render_data_restricted_rays.cpp
 *
 * Copyright 2018 by Intel.
 *
 * Contact: kevin.rogovin@intel.com
 *
 * This Source Code Form is subject to the
 * terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with
 * this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * \author Kevin Rogovin <kevin.rogovin@intel.com>
 *
 */


#include <vector>
#include <algorithm>
#include <map>
#include <fastuidraw/util/matrix.hpp>
#include <fastuidraw/text/glyph_render_data_restricted_rays.hpp>
#include "../private/bounding_box.hpp"
#include "../private/util_private.hpp"
#include "../private/util_private_ostream.hpp"

namespace
{
  enum
    {
      MAX_RECURSION = 12,

      /* number of curves in a texel that we stop splitting at */
      SPLIT_THRESH = 4,
    };

  inline
  uint32_t
  bias_winding(int w)
  {
    typedef fastuidraw::GlyphRenderDataRestrictedRays G;

    w += G::winding_bias;
    FASTUIDRAWassert(w >= 0);
    FASTUIDRAWassert(w <= int(FASTUIDRAW_MAX_VALUE_FROM_NUM_BITS(G::winding_value_numbits)));

    return uint32_t(w);
  }

  inline
  uint32_t
  pack_point(fastuidraw::ivec2 p)
  {
    typedef fastuidraw::GlyphRenderDataRestrictedRays G;
    uint32_t x, y;

    FASTUIDRAWassert(p.x() >= 0);
    FASTUIDRAWassert(p.x() <= int(FASTUIDRAW_MAX_VALUE_FROM_NUM_BITS(G::point_coordinate_numbits)));
    FASTUIDRAWassert(p.y() >= 0);
    FASTUIDRAWassert(p.y() <= int(FASTUIDRAW_MAX_VALUE_FROM_NUM_BITS(G::point_coordinate_numbits)));

    x = fastuidraw::pack_bits(G::point_x_coordinate_bit0,
                              G::point_coordinate_numbits,
                              uint32_t(p.x()));

    y = fastuidraw::pack_bits(G::point_y_coordinate_bit0,
                              G::point_coordinate_numbits,
                              uint32_t(p.y()));
    return x | y;
  }

  inline
  fastuidraw::ivec2
  unpack_point(uint32_t v)
  {
    typedef fastuidraw::GlyphRenderDataRestrictedRays G;
    fastuidraw::ivec2 r;

    r.x() = fastuidraw::unpack_bits(G::point_x_coordinate_bit0,
                                    G::point_coordinate_numbits, v);
    r.y() = fastuidraw::unpack_bits(G::point_y_coordinate_bit0,
                                    G::point_coordinate_numbits, v);
    return r;
  }

  class GlyphPath;

  class Curve
  {
  public:
    Curve(const fastuidraw::ivec2 &a,
          const fastuidraw::ivec2 &b):
      m_offset(-1),
      m_start(a),
      m_end(b),
      m_control((a + b) / 2),
      m_fstart(a),
      m_fend(b),
      m_fcontrol((m_fstart + m_fend) * 0.5f),
      m_has_control(false)
    {
      m_fbbox.union_point(m_fstart);
      m_fbbox.union_point(m_fend);
    }

    Curve(const fastuidraw::ivec2 &a,
          const fastuidraw::ivec2 &ct,
          const fastuidraw::ivec2 &b):
      m_offset(-1),
      m_start(a),
      m_end(b),
      m_control(ct),
      m_fstart(a),
      m_fend(b),
      m_fcontrol(ct),
      m_has_control(!is_flat(a, ct, b))
    {
      m_fbbox.union_point(m_fstart);
      m_fbbox.union_point(m_fcontrol);
      m_fbbox.union_point(m_fend);
    }

    Curve(fastuidraw::ivec2 scale_down, const Curve &C):
      m_offset(-1),
      m_start(C.m_start / scale_down),
      m_end(C.m_end / scale_down),
      m_control(C.m_control / scale_down),
      m_fstart(m_start),
      m_fend(m_end),
      m_fcontrol(m_control),
      m_has_control(C.m_has_control && !is_flat(m_start, m_control, m_end))
    {
      FASTUIDRAWassert(C.m_offset == -1);
      m_fbbox.union_point(m_fstart);
      m_fbbox.union_point(m_fend);
      if (m_has_control)
        {
          m_fbbox.union_point(m_fcontrol);
        }
    }

    const fastuidraw::ivec2&
    start(void) const { return m_start; }

    const fastuidraw::ivec2&
    end(void) const { return m_end; }

    const fastuidraw::ivec2&
    control(void) const
    {
      FASTUIDRAWassert(m_has_control);
      return m_control;
    }

    const fastuidraw::vec2&
    fstart(void) const { return m_fstart; }

    const fastuidraw::vec2&
    fend(void) const { return m_fend; }

    const fastuidraw::vec2&
    fcontrol(void) const
    {
      FASTUIDRAWassert(m_has_control);
      return m_fcontrol;
    }

    bool
    has_control(void) const
    {
      return m_has_control;
    }

    void
    pack_data(fastuidraw::c_array<fastuidraw::generic_data> dst) const
    {
      FASTUIDRAWassert(m_offset >= 0);

      unsigned int p(m_offset);
      dst[p++].u = pack_point(start());
      if (has_control())
        {
          dst[p++].u = pack_point(control());
        }
      dst[p++].u = pack_point(end());
    }

    int
    compute_winding_contribution(fastuidraw::vec2 xy) const;

    friend
    std::ostream&
    operator<<(std::ostream &ostr, const Curve &C)
    {
      ostr << "[" << C.start() << " ";
      if (C.has_control())
        {
          ostr << "Ct" << C.control() << " ";
        }
      ostr << C.end() << "]";
      return ostr;
    }

    bool
    intersects(const fastuidraw::BoundingBox<float> &box) const;

    void
    translate(const fastuidraw::ivec2 &v)
    {
      fastuidraw::vec2 fv(v);

      m_start += v;
      m_end += v;
      m_control += v;
      m_fstart += fv;
      m_fend += fv;
      m_fcontrol += fv;
      m_fbbox.translate(fv);
    }

    /* Assigned in GlyphRenderDataRestrictedRays::finalize() */
    int m_offset;

  private:
    static
    bool
    is_flat(fastuidraw::ivec2 a,
                 fastuidraw::ivec2 b,
                 fastuidraw::ivec2 c)
    {
      fastuidraw::i64vec2 v(b - a), w(c - a);
      int64_t det;

      det = v.x() * w.y() - v.y() * w.x();
      return (det == 0);
    }

    float
    compute_at(float t, int coordinate) const;

    bool
    intersects_edge_test_at_time(float t, int coordinate, float min, float max) const;

    bool
    intersects_edge(float v, int coordinate, float min, float max) const;

    bool
    intersects_vertical_edge(float x, float miny, float maxy) const
    {
      return intersects_edge(x, 0, miny, maxy);
    }

    bool
    intersects_horizontal_edge(float y, float minx, float maxx) const
    {
      return intersects_edge(y, 1, minx, maxx);
    }

    fastuidraw::ivec2 m_start, m_end, m_control;
    fastuidraw::vec2 m_fstart, m_fend, m_fcontrol;
    fastuidraw::BoundingBox<float> m_fbbox;
    bool m_has_control;
  };

  class Contour
  {
  public:
    explicit
    Contour(fastuidraw::ivec2 pt):
      m_last_pt(pt),
      m_num_pts(1)
    {}

    Contour(fastuidraw::ivec2 scale_down,
            const Contour &contour)
    {
      for (const Curve &C : contour.m_curves)
        {
          Curve D(scale_down, C);
          if (D.start() != D.end())
            {
              m_curves.push_back(D);
            }
        }
    }

    void
    line_to(fastuidraw::ivec2 pt)
    {
      if (m_last_pt != pt)
        {
          m_curves.push_back(Curve(m_last_pt, pt));
          m_last_pt = pt;
          ++m_num_pts;
        }
    }

    void
    quadratic_to(fastuidraw::ivec2 ct,
                 fastuidraw::ivec2 pt)
    {
      if (m_last_pt != pt)
        {
          m_curves.push_back(Curve(m_last_pt, ct, pt));
          m_last_pt = pt;
          m_num_pts += 2;
        }
    }

    unsigned int
    num_points(void) const
    {
      return m_num_pts;
    }

    bool
    is_good(void) const
    {
      return !m_curves.empty()
        && m_curves.front().start() == m_curves.back().end();
    }

    const Curve&
    operator[](unsigned int C) const
    {
      FASTUIDRAWassert(C < m_curves.size());
      return m_curves[C];
    }

    void
    translate(const fastuidraw::ivec2 &v)
    {
      for (Curve &C : m_curves)
        {
          C.translate(v);
        }
    }

    unsigned int
    num_curves(void) const
    {
      return m_curves.size();
    }

    bool
    empty(void) const
    {
      return m_curves.empty();
    }

    void
    assign_curve_offsets(unsigned int &current_offset);

    void
    pack_data(fastuidraw::c_array<fastuidraw::generic_data> dst) const;

  private:
    fastuidraw::ivec2 m_last_pt;
    std::vector<Curve> m_curves;
    unsigned int m_num_pts;
  };

  class CurveID
  {
  public:
    CurveID& contour(int v) { m_ID.x() = v; return *this; }
    CurveID& curve(int v) { m_ID.y() = v; return *this; }
    unsigned int contour(void) const { return m_ID.x(); }
    unsigned int curve(void) const { return m_ID.y(); }
    bool operator<(const CurveID &rhs) const { return m_ID < rhs.m_ID; }
  private:
    fastuidraw::uvec2 m_ID;
  };

  class CurveList
  {
  public:
    CurveList(void):
      m_offset(-1),
      m_p(nullptr)
    {}

    unsigned int
    init(const GlyphPath *p,
         const fastuidraw::vec2 &min_pt,
         const fastuidraw::vec2 &max_pt);

    unsigned int //returns the splitting coordinate
    split(CurveList &out_pre, CurveList &out_post) const;

    const std::vector<CurveID>&
    curves(void) const
    {
      FASTUIDRAWassert(m_p);
      return m_curves;
    }

    const fastuidraw::BoundingBox<float>&
    box(void) const
    {
      return m_box;
    }

    const GlyphPath&
    glyph_path(void) const { return *m_p; }

    /* Assigned in GlyphRenderDataRestrictedRays::finalize() */
    int m_offset;

  private:
    const GlyphPath *m_p;
    std::vector<CurveID> m_curves;
    fastuidraw::BoundingBox<float> m_box;
  };

  class CurveListPacker
  {
  public:
    CurveListPacker(fastuidraw::c_array<fastuidraw::generic_data> dst,
                    unsigned int offset):
      m_dst(dst),
      m_current_offset(offset),
      m_sub_offset(0u),
      m_current_value(0u)
    {}

    void
    add_curve(uint32_t location, bool is_quadratic);

    void
    end_list(void);

    static
    unsigned int
    room_required(unsigned int num_curves);

    static
    void
    print_curve_list(unsigned int cnt,
                     fastuidraw::c_array<const fastuidraw::generic_data> src,
                     unsigned int offset, std::string prefix);

  private:
    fastuidraw::c_array<fastuidraw::generic_data> m_dst;
    unsigned int m_current_offset, m_sub_offset;
    uint32_t m_current_value;
  };

  class CurveListCollection
  {
  public:
    explicit
    CurveListCollection(unsigned int start_offset):
      m_start_offset(start_offset),
      m_current_offset(start_offset)
    {}

    unsigned int
    start_offset(void) const
    {
      return m_start_offset;
    }

    unsigned int
    current_offset(void) const
    {
      return m_current_offset;
    }

    void
    assign_offset(CurveList &C);

    void
    pack_data(const GlyphPath *p,
              fastuidraw::c_array<fastuidraw::generic_data> data) const;

  private:
    void
    pack_element(const GlyphPath *p,
                 const std::vector<CurveID> &curves,
                 unsigned int offset,
                 fastuidraw::c_array<fastuidraw::generic_data> data) const;

    unsigned int m_start_offset, m_current_offset;
    std::map<std::vector<CurveID>, unsigned int> m_offsets;
  };

  class CurveListHierarchy
  {
  public:
    CurveListHierarchy(const GlyphPath *p,
                       const fastuidraw::ivec2 &min_pt,
                       const fastuidraw::ivec2 &max_pt):
      m_child(nullptr, nullptr),
      m_offset(-1),
      m_splitting_coordinate(3),
      m_generation(0)
    {
      m_curves.init(p,
                    fastuidraw::vec2(min_pt),
                    fastuidraw::vec2(max_pt));
      subdivide();
    }

    ~CurveListHierarchy()
    {
      if (has_children())
        {
          FASTUIDRAWdelete(m_child[0]);
          FASTUIDRAWdelete(m_child[1]);
        }
    }

    unsigned int
    assign_tree_offsets(void)
    {
      unsigned int size(0);

      assign_tree_offsets(size);
      return size;
    }

    void
    print_tree(std::string prefix) const;

    void
    assign_curve_list_offsets(CurveListCollection &C);

    void
    pack_data(fastuidraw::c_array<fastuidraw::generic_data> dst) const;

  private:
    explicit
    CurveListHierarchy(unsigned int parent_gen):
      m_child(nullptr, nullptr),
      m_offset(-1),
      m_splitting_coordinate(3),
      m_generation(parent_gen + 1)
    {}

    void
    subdivide(void);

    void
    assign_tree_offsets(unsigned int &start);

    bool
    has_children(void) const
    {
      FASTUIDRAWassert((m_child[0] == nullptr) == (m_child[1] == nullptr));
      return m_child[0] != nullptr;
    }

    void
    pack_texel(fastuidraw::c_array<fastuidraw::generic_data> dst) const;

    void
    pack_sample_point(unsigned int &offset,
                      fastuidraw::c_array<fastuidraw::generic_data> dst) const;

    fastuidraw::vecN<CurveListHierarchy*, 2> m_child;
    unsigned int m_offset, m_splitting_coordinate, m_generation;
    CurveList m_curves;

    int m_winding;
    fastuidraw::ivec2 m_delta;
  };

  class GlyphPath
  {
  public:
    GlyphPath(void);

    int
    compute_winding_number(fastuidraw::vec2 xy) const;

    void
    move_to(const fastuidraw::ivec2 &pt)
    {
      if (!m_contours.empty() && m_contours.back().empty())
        {
          m_contours.pop_back();
        }
      FASTUIDRAWassert(m_contours.empty() || m_contours.back().is_good());
      m_bbox.union_point(pt);
      m_contours.push_back(Contour(pt));
    }

    void
    quadratic_to(const fastuidraw::ivec2 &ct,
                 const fastuidraw::ivec2 &pt)
    {
      FASTUIDRAWassert(!m_contours.empty());
      m_bbox.union_point(ct);
      m_bbox.union_point(pt);
      m_contours.back().quadratic_to(ct, pt);
    }

    void
    line_to(const fastuidraw::ivec2 &pt)
    {
      FASTUIDRAWassert(!m_contours.empty());
      m_bbox.union_point(pt);
      m_contours.back().line_to(pt);
    }

    const std::vector<Contour>&
    contours(void) const
    {
      return m_contours;
    }

    unsigned int
    assign_curve_offsets(unsigned int start_offset);

    void
    pack_data(fastuidraw::c_array<fastuidraw::generic_data> dst) const;

    const Curve&
    fetch_curve(const CurveID &id) const
    {
      FASTUIDRAWassert(id.contour() < m_contours.size());
      FASTUIDRAWassert(id.curve() < m_contours[id.contour()].num_curves());
      return m_contours[id.contour()][id.curve()];
    }

    void
    translate(const fastuidraw::ivec2 &v)
    {
      for (Contour &C : m_contours)
        {
          C.translate(v);
        }
      m_bbox.translate(v);
      m_glyph_bound_min += v;
      m_glyph_bound_max += v;
    }

    void
    scale_down(const fastuidraw::ivec2 &v)
    {
      std::vector<Contour> tmp;

      std::swap(tmp, m_contours);
      for (const Contour &C : tmp)
        {
          Contour D(v, C);
          if (!D.empty())
            {
              m_contours.push_back(D);
            }
        }
      m_bbox.scale_down(v);
      m_glyph_bound_min /= v;
      m_glyph_bound_max /= v;
    }

    void
    set_glyph_bounds(fastuidraw::ivec2 min_pt,
                     fastuidraw::ivec2 max_pt)
    {
      m_glyph_bound_min = min_pt;
      m_glyph_bound_max = max_pt;
      m_bbox.union_point(min_pt);
      m_bbox.union_point(max_pt);
    }

    const fastuidraw::BoundingBox<int>&
    bbox(void) const
    {
      return m_bbox;
    }

    const fastuidraw::ivec2&
    glyph_bound_min(void) const
    {
      return m_glyph_bound_min;
    }

    const fastuidraw::ivec2&
    glyph_bound_max(void) const
    {
      return m_glyph_bound_max;
    }

  private:
    fastuidraw::BoundingBox<int> m_bbox;
    fastuidraw::ivec2 m_glyph_bound_min, m_glyph_bound_max;
    std::vector<Contour> m_contours;
  };

  void
  print_curve(unsigned int id,
              fastuidraw::c_array<const fastuidraw::generic_data> data,
              unsigned int p, const std::string &prefix)
  {
    const uint32_t mask(1u << 15u);
    uint32_t offset(p & ~mask);

    std::cout << prefix << "(" << id << ") ["
              << unpack_point(data[offset++].u);
    if (p & mask)
      {
        std::cout << " Ct" << unpack_point(data[offset++].u);
      }
    std::cout << " " << unpack_point(data[offset++].u) << "]\n";
  }

  void
  print_tree_from_packed(unsigned int offset, fastuidraw::BoundingBox<float> box,
                         fastuidraw::c_array<const fastuidraw::generic_data> data,
                         std::string prefix)
  {
    typedef fastuidraw::GlyphRenderDataRestrictedRays G;
    uint32_t v;
    bool has_children;

    prefix += "\t";

    v = data[offset].u;
    has_children = (fastuidraw::unpack_bits(G::hierarchy_is_node_bit, 1u, v) == 1u);

    std::cout << prefix << "BoundingBox" << box.min_point()
              << " -- " << box.max_point() << "@" << offset << ":";
    if (has_children)
      {
        uint32_t coord, pre, post;
        fastuidraw::vecN<fastuidraw::BoundingBox<float>, 2> split;

        coord = fastuidraw::unpack_bits(G::hierarchy_splitting_coordinate_bit, 1u, v);
        pre = fastuidraw::unpack_bits(G::hierarchy_child0_offset_bit0,
                                      G::hierarchy_child_offset_numbits, v);
        post = fastuidraw::unpack_bits(G::hierarchy_child1_offset_bit0,
                                       G::hierarchy_child_offset_numbits, v);
        if (coord == 0)
          {
            std::cout << "SplitX:\n";
            split = box.split_x();
          }
        else
          {
            std::cout << "SplitY:\n";
            split = box.split_y();
          }
        print_tree_from_packed(pre, split[0], data, prefix);
        print_tree_from_packed(post, split[1], data, prefix);
      }
    else
      {
        uint32_t curve_list, curve_list_size;
        unsigned int biased_winding;
        fastuidraw::ivec2 delta;

        curve_list = fastuidraw::unpack_bits(G::hierarchy_leaf_curve_list_bit0,
                                             G::hierarchy_leaf_curve_list_numbits,
                                             v);
        curve_list_size = fastuidraw::unpack_bits(G::hierarchy_leaf_curve_list_size_bit0,
                                                  G::hierarchy_leaf_curve_list_size_numbits,
                                                  v);
        ++offset;
        v = data[offset].u;
        biased_winding = fastuidraw::unpack_bits(G::winding_value_bit0,
                                                 G::winding_value_numbits, v);
        delta.x() = fastuidraw::unpack_bits(G::delta_x_bit0, G::delta_numbits, v);
        delta.y() = fastuidraw::unpack_bits(G::delta_y_bit0, G::delta_numbits, v);
        delta -= fastuidraw::ivec2(G::delta_bias);

        std::cout << "\n" << prefix << "winding = "
                  << int(biased_winding) - int(G::winding_bias)
                  << "\n" << prefix << "delta = " << delta << "\n";

        if (curve_list_size != 0u)
          {
            std::cout << prefix << "curve_list@" << curve_list
                      << " with " << curve_list_size
                      << " elements\n";
            CurveListPacker::print_curve_list(curve_list_size, data, curve_list, prefix);
          }
        else
          {
            std::cout << ", empty curve_list\n";
          }
      }
  }

  /* The main trickiness we have hear is that we store
   * the input point times two (forcing it to be even).
   * By doing so, we can always make the needed control
   * point for line_to().
   */
  class GlyphRenderDataRestrictedRaysPrivate
  {
  public:
    GlyphRenderDataRestrictedRaysPrivate(void)
    {
      m_glyph = FASTUIDRAWnew GlyphPath();
    }

    ~GlyphRenderDataRestrictedRaysPrivate(void)
    {
      if (m_glyph)
        {
          FASTUIDRAWdelete(m_glyph);
        }
    }

    GlyphPath *m_glyph;
    fastuidraw::ivec2 m_min, m_max, m_size;
    enum fastuidraw::PainterEnums::fill_rule_t m_fill_rule;
    std::vector<fastuidraw::generic_data> m_render_data;
  };
}

////////////////////////////////////////////////
// Curve methods
int
Curve::
compute_winding_contribution(fastuidraw::vec2 p) const
{
  using namespace fastuidraw;
  vec2 p1(m_fstart - p), p2(m_fcontrol - p), p3(m_fend - p);
  uint32_t code;

  code = (p1.y() > 0.0f ? 2u : 0u)
    + (p2.y() > 0.0f ? 4u : 0u)
    + (p3.y() > 0.0f ? 8u : 0u);

  code = (0x2E74u >> code) & 0x3u;
  if (code == 0u)
    {
      return 0;
    }

  int iA;
  vec2 A, B, C;
  float t1, t2, x1, x2;

  iA = m_start.y() - 2 * m_control.y() + m_end.y();
  A = p1 - 2.0 * p2 + p3;
  B = p1 - p2;
  C = p1;

  if (m_has_control && iA != 0)
    {
      float D, rA = 1.0f / A.y();

      D = B.y() * B.y() - A.y() * C.y();
      if (D < 0.0f)
        {
          return 0;
        }

      D = t_sqrt(D);
      t1 = (B.y() - D) * rA;
      t2 = (B.y() + D) * rA;
    }
  else
    {
      t1 = t2 = 0.5f * C.y() / B.y();
    }

  x1 = (A.x() * t1 - B.x() * 2.0f) * t1 + C.x();
  x2 = (A.x() * t2 - B.x() * 2.0f) * t2 + C.x();

  int r(0);
  if ((code & 1u) != 0u && x1 > 0.0f)
    {
      ++r;
    }

  if (code > 1u && x2 > 0.0f)
    {
      --r;
    }

  return r;
}

bool
Curve::
intersects(const fastuidraw::BoundingBox<float> &box) const
{
  if (!m_fbbox.intersects(box))
    {
      return false;
    }

  if (box.contains(m_fstart)
      || box.contains(m_fend))
    {
      return true;
    }

  return intersects_vertical_edge(box.min_point().x(), box.min_point().y(), box.max_point().y())
    || intersects_vertical_edge(box.max_point().x(), box.min_point().y(), box.max_point().y())
    || intersects_horizontal_edge(box.min_point().y(), box.min_point().x(), box.max_point().x())
    || intersects_horizontal_edge(box.max_point().y(), box.min_point().x(), box.max_point().x());

  /* TODO:
   *   - scheme to save the polynomial solves computed.
   */
}

float
Curve::
compute_at(float t, int d) const
{
  float A, B, C;

  A = m_fstart[d] - 2.0f * m_fcontrol[d] + m_fend[d];
  B = m_fstart[d] - m_fcontrol[d];
  C = m_fstart[d];

  return (A * t - B * 2.0f) * t + C;
}

bool
Curve::
intersects_edge_test_at_time(float t, int coordinate, float min, float max) const
{
  float v;

  v = compute_at(t, 1 - coordinate);
  return v >= min && v <= max;
}

bool
Curve::
intersects_edge(float v, int i, float min, float max) const
{
  float A, B, C;
  int iA;

  iA = m_start[i] - 2 * m_control[i] + m_end[i];
  A = m_fstart[i] - 2.0f * m_fcontrol[i] + m_fend[i];
  B = m_fstart[i] - m_fcontrol[i];
  C = m_fstart[i] - v;

  if (m_has_control && iA != 0)
    {
      float t1, t2, D, rA = 1.0f / A;

      D = B * B - A * C;
      if (D < 0.0f)
        {
          return false;
        }

      D = fastuidraw::t_sqrt(D);
      t1 = (B - D) * rA;
      t2 = (B + D) * rA;

      return intersects_edge_test_at_time(t1, i, min, max)
        || intersects_edge_test_at_time(t2, i, min, max);
    }
  else
    {
      float t;
      t = 0.5f * C / B;
      return intersects_edge_test_at_time(t, i, min, max);
    }
}

/////////////////////////////////////////
// Contour methods
void
Contour::
assign_curve_offsets(unsigned int &current_offset)
{
  FASTUIDRAWassert(is_good());
  for (Curve &curve : m_curves)
    {
      unsigned int cnt;

      curve.m_offset = current_offset;
      cnt = (curve.has_control()) ? 2 : 1;
      current_offset += cnt;
    }

  /* the last point of the last curve which is the same as
   * the first point of the first curve needs to be added
   * so that that the last curve access has the point
   */
  ++current_offset;
}

void
Contour::
pack_data(fastuidraw::c_array<fastuidraw::generic_data> dst) const
{
  for (const Curve &curve : m_curves)
    {
      curve.pack_data(dst);
    }
}

//////////////////////////////////////
// CurveList methods
unsigned int
CurveList::
init(const GlyphPath *p,
     const fastuidraw::vec2 &min_pt,
     const fastuidraw::vec2 &max_pt)
{
  FASTUIDRAWassert(!m_p);
  m_p = p;
  m_box = fastuidraw::BoundingBox<float>(min_pt, max_pt);

  const std::vector<Contour> &g(m_p->contours());
  for (unsigned int o = 0, endo = g.size(); o < endo; ++o)
    {
      for (unsigned int c = 0, endc = g[o].num_curves(); c < endc; ++c)
        {
          m_curves.push_back(CurveID()
                             .curve(c)
                             .contour(o));
        }
    }
  return m_curves.size();
}

unsigned int
CurveList::
split(CurveList &out_pre, CurveList &out_post) const
{
  using namespace fastuidraw;
  vecN<std::vector<CurveID>, 2> splitX;
  vecN<std::vector<CurveID>, 2> splitY;
  vecN<BoundingBox<float>, 2> splitX_box(m_box.split_x());
  vecN<BoundingBox<float>, 2> splitY_box(m_box.split_y());

  /* choose the partition with the smallest sum of curves */
  FASTUIDRAWassert(std::is_sorted(m_curves.begin(), m_curves.end()));
  for (const CurveID &id : m_curves)
    {
      const Curve &curve(m_p->fetch_curve(id));
      for (int i = 0; i < 2; ++i)
        {
          if (curve.intersects(splitX_box[i]))
            {
              splitX[i].push_back(id);
            }
          if (curve.intersects(splitY_box[i]))
            {
              splitY[i].push_back(id);
            }
        }
    }

  out_pre.m_p = m_p;
  out_post.m_p = m_p;

  if (splitX[0].size() + splitX[1].size() < splitY[0].size() + splitY[1].size())
    {
      std::swap(out_pre.m_curves, splitX[0]);
      std::swap(out_post.m_curves, splitX[1]);
      out_pre.m_box = splitX_box[0];
      out_post.m_box = splitX_box[1];
      return 0;
    }
  else
    {
      std::swap(out_pre.m_curves, splitY[0]);
      std::swap(out_post.m_curves, splitY[1]);
      out_pre.m_box = splitY_box[0];
      out_post.m_box = splitY_box[1];
      return 1;
    }
}

//////////////////////////////////
// CurveListPacker methods
void
CurveListPacker::
add_curve(uint32_t curve_location, bool curve_is_quadratic)
{
  typedef fastuidraw::GlyphRenderDataRestrictedRays G;
  uint32_t v(curve_location);

  if (curve_is_quadratic)
    {
      v |= (1u << G::curve_is_quadratic_bit);
    }

  if (m_sub_offset == 0u)
    {
      m_current_value = (v << G::curve_entry0_bit0);
      ++m_sub_offset;
    }
  else
    {
      FASTUIDRAWassert(m_sub_offset == 1);

      m_dst[m_current_offset++].u = m_current_value | (v << G::curve_entry1_bit0);
      m_current_value = 0u;
      m_sub_offset = 0;
    }
}

unsigned int
CurveListPacker::
room_required(unsigned int cnt)
{
  /* round up to even */
  cnt = (cnt + 1u) & ~1u;

  /* every 2 curves takes takes an entry */
  cnt = (cnt >> 1u);

  return cnt;
}

void
CurveListPacker::
end_list(void)
{
  if (m_sub_offset == 1)
    {
      m_dst[m_current_offset++].u = m_current_value;
    }
}

void
CurveListPacker::
print_curve_list(unsigned int cnt,
                 fastuidraw::c_array<const fastuidraw::generic_data> data,
                 unsigned int offset, std::string prefix)
{
  typedef fastuidraw::GlyphRenderDataRestrictedRays G;

  prefix += "\t";
  for (unsigned int i = 0; i < cnt; i += 2)
    {
      uint32_t v, c0, c1;

      v = data[offset++].u;
      c0 = fastuidraw::unpack_bits(G::curve_entry0_bit0, G::curve_numbits, v);
      c1 = fastuidraw::unpack_bits(G::curve_entry1_bit0, G::curve_numbits, v);

      print_curve(i, data, c0, prefix);
      if (i + 1 < cnt)
        {
          print_curve(i + 1, data, c1, prefix);
        }
    }
}

///////////////////////////////////////////
// CurveListCollection methods
void
CurveListCollection::
assign_offset(CurveList &C)
{
  if (C.curves().empty())
    {
      C.m_offset = 0;
      return;
    }

  std::map<std::vector<CurveID>, unsigned int>::iterator iter;

  iter = m_offsets.find(C.curves());
  if (iter != m_offsets.end())
    {
      C.m_offset = iter->second;
    }
  else
    {
      C.m_offset = m_current_offset;
      iter = m_offsets.insert(std::make_pair(C.curves(), C.m_offset)).first;
      m_current_offset += CurveListPacker::room_required(C.curves().size());
    }
}

void
CurveListCollection::
pack_data(const GlyphPath *p,
          fastuidraw::c_array<fastuidraw::generic_data> data) const
{
  for (const auto &element : m_offsets)
    {
      pack_element(p, element.first, element.second, data);
    }
}

void
CurveListCollection::
pack_element(const GlyphPath *p,
             const std::vector<CurveID> &curves,
             unsigned int offset,
             fastuidraw::c_array<fastuidraw::generic_data> dst) const
{
  CurveListPacker curve_list_packer(dst, offset);

  FASTUIDRAWassert(!curves.empty());
  FASTUIDRAWassert(std::is_sorted(curves.begin(), curves.end()));
  for (const CurveID &id : curves)
    {
      const Curve &curve(p->fetch_curve(id));
      bool has_ctl(curve.has_control());

      FASTUIDRAWassert(curve.m_offset > 0);
      curve_list_packer.add_curve(curve.m_offset, has_ctl);
    }

  curve_list_packer.end_list();
}

///////////////////////////////////
// CurveListHierarchy methods
void
CurveListHierarchy::
pack_sample_point(unsigned int &offset,
                  fastuidraw::c_array<fastuidraw::generic_data> dst) const
{
  using namespace fastuidraw;
  typedef GlyphRenderDataRestrictedRays G;

  /* bits 0-15 for winding, biased
   * bits 16-23 for dx, biased by G::delta_bias
   * bits 24-31 for dy, biased by G::delta_bias
   */
  dst[offset++].u = pack_bits(G::winding_value_bit0,
                              G::winding_value_numbits,
                              bias_winding(m_winding))
    | pack_bits(G::delta_x_bit0, G::delta_numbits, m_delta.x() + G::delta_bias)
    | pack_bits(G::delta_y_bit0, G::delta_numbits, m_delta.y() + G::delta_bias);
}

void
CurveListHierarchy::
pack_texel(fastuidraw::c_array<fastuidraw::generic_data> dst) const
{
  using namespace fastuidraw;
  typedef GlyphRenderDataRestrictedRays G;

  if (!has_children())
    {
      unsigned int offset(m_offset);

      dst[offset++].u = pack_bits(G::hierarchy_leaf_curve_list_bit0,
                                  G::hierarchy_leaf_curve_list_numbits,
                                  m_curves.m_offset)
        | pack_bits(G::hierarchy_leaf_curve_list_size_bit0,
                    G::hierarchy_leaf_curve_list_size_numbits,
                    m_curves.curves().size());

      pack_sample_point(offset, dst);
    }
  else
    {
      FASTUIDRAWassert(m_splitting_coordinate <= 1u);

      /* pack the splitting data */
      dst[m_offset].u =
        (1u << G::hierarchy_is_node_bit) // bit flag to indicate a node
        | (m_splitting_coordinate << G::hierarchy_splitting_coordinate_bit) // splitting coordinate
        | pack_bits(G::hierarchy_child0_offset_bit0,
                    G::hierarchy_child_offset_numbits,
                    m_child[0]->m_offset) // pre-split child
        | pack_bits(G::hierarchy_child1_offset_bit0,
                    G::hierarchy_child_offset_numbits,
                    m_child[1]->m_offset); // post-split child
    }
}

void
CurveListHierarchy::
pack_data(fastuidraw::c_array<fastuidraw::generic_data> dst) const
{
  pack_texel(dst);
  if (has_children())
    {
      m_child[0]->pack_data(dst);
      m_child[1]->pack_data(dst);
    }
}

void
CurveListHierarchy::
assign_tree_offsets(unsigned int &current)
{
  m_offset = current++;
  if (has_children())
    {
      m_child[0]->assign_tree_offsets(current);
      m_child[1]->assign_tree_offsets(current);
    }
  else
    {
      /* room needed for the sample point */
      current += 1u;
    }
}

void
CurveListHierarchy::
assign_curve_list_offsets(CurveListCollection &C)
{
  if (has_children())
    {
      m_child[0]->assign_curve_list_offsets(C);
      m_child[1]->assign_curve_list_offsets(C);
    }
  else
    {
      C.assign_offset(m_curves);
    }
}

void
CurveListHierarchy::
subdivide(void)
{
  using namespace fastuidraw;
  if (m_generation < MAX_RECURSION && m_curves.curves().size() > SPLIT_THRESH)
    {
      m_child[0] = FASTUIDRAWnew CurveListHierarchy(m_generation);
      m_child[1] = FASTUIDRAWnew CurveListHierarchy(m_generation);

      m_splitting_coordinate = m_curves.split(m_child[0]->m_curves,
                                              m_child[1]->m_curves);
      m_child[0]->subdivide();
      m_child[1]->subdivide();

      if (!m_child[0]->has_children()
          && !m_child[1]->has_children()
          && m_child[0]->m_curves.curves().size() == m_curves.curves().size()
          && m_child[1]->m_curves.curves().size() == m_curves.curves().size())
        {
          /* both children have no children and
           * the same curve list, thus there is
           * no point of subdivding this node
           */
          FASTUIDRAWdelete(m_child[0]); m_child[0] = nullptr;
          FASTUIDRAWdelete(m_child[1]); m_child[1] = nullptr;
          m_splitting_coordinate = 3;
        }
    }

  if (!has_children())
    {
      /* TODO: Gaurantee that the sample point chosen does
       *       NOT have any of the curves go through it.
       */
      m_delta = ivec2(1, 3);

      vec2 s, delta;

      delta = vec2(m_delta) / float(GlyphRenderDataRestrictedRays::delta_div_factor);
      s = m_curves.box().center_point() + delta ;
      m_winding = m_curves.glyph_path().compute_winding_number(s);
    }
}

void
CurveListHierarchy::
print_tree(std::string prefix) const
{
  prefix += "\t";
  std::cout << prefix << "BoundingBox" << m_curves.box().min_point()
            << " -- " << m_curves.box().max_point() << "@" << m_offset << ":";
  if (has_children())
    {
      if (m_splitting_coordinate == 0u)
        {
          std::cout << "SplitX:\n";
        }
      else
        {
          std::cout << "SplitY:\n";
        }
      m_child[0]->print_tree(prefix);
      m_child[1]->print_tree(prefix);
    }
  else
    {
      unsigned int cnt(0);

      std::cout << "\n" << prefix << "winding = " << m_winding << "\n"
                << prefix << "delta = " << m_delta << "\n"
                << prefix << "curve_list with " << m_curves.curves().size() << " elements\n";
      prefix += "\t";

      cnt = 0;
      for (const CurveID &id : m_curves.curves())
        {
          std::cout << prefix << "(" << cnt << ") "
                    << m_curves.glyph_path().fetch_curve(id)
                    << "\n";
          ++cnt;
        }
    }
}

////////////////////////////////////////////////
//GlyphPath methods
GlyphPath::
GlyphPath(void)
{
}

unsigned int
GlyphPath::
assign_curve_offsets(unsigned int current_offset)
{
  for (Contour &C : m_contours)
    {
      C.assign_curve_offsets(current_offset);
    }
  return current_offset;
}

void
GlyphPath::
pack_data(fastuidraw::c_array<fastuidraw::generic_data> dst) const
{
  for (const Contour &C : m_contours)
    {
      C.pack_data(dst);
    }
}

int
GlyphPath::
compute_winding_number(fastuidraw::vec2 xy) const
{
  /* just compute it from scratch */
  int w(0);
  for (const Contour &contour : m_contours)
    {
      for (unsigned int i = 0, endi = contour.num_curves(); i < endi; ++i)
        {
          w += contour[i].compute_winding_contribution(xy);
        }
    }

  return w;
}

/////////////////////////////////////////////////
// fastuidraw::GlyphRenderDataRestrictedRays methods
fastuidraw::GlyphRenderDataRestrictedRays::
GlyphRenderDataRestrictedRays(void)
{
  m_d = FASTUIDRAWnew GlyphRenderDataRestrictedRaysPrivate();
}

fastuidraw::GlyphRenderDataRestrictedRays::
~GlyphRenderDataRestrictedRays()
{
  GlyphRenderDataRestrictedRaysPrivate *d;

  d = static_cast<GlyphRenderDataRestrictedRaysPrivate*>(m_d);
  FASTUIDRAWdelete(d);
}

void
fastuidraw::GlyphRenderDataRestrictedRays::
move_to(ivec2 pt)
{
  GlyphRenderDataRestrictedRaysPrivate *d;

  d = static_cast<GlyphRenderDataRestrictedRaysPrivate*>(m_d);
  FASTUIDRAWassert(d->m_glyph);
  d->m_glyph->move_to(pt);
}

void
fastuidraw::GlyphRenderDataRestrictedRays::
quadratic_to(ivec2 ct, ivec2 pt)
{
  GlyphRenderDataRestrictedRaysPrivate *d;

  d = static_cast<GlyphRenderDataRestrictedRaysPrivate*>(m_d);
  FASTUIDRAWassert(d->m_glyph);
  d->m_glyph->quadratic_to(ct, pt);
}

void
fastuidraw::GlyphRenderDataRestrictedRays::
line_to(ivec2 pt)
{
  GlyphRenderDataRestrictedRaysPrivate *d;

  d = static_cast<GlyphRenderDataRestrictedRaysPrivate*>(m_d);
  FASTUIDRAWassert(d->m_glyph);
  d->m_glyph->line_to(pt);
}

void
fastuidraw::GlyphRenderDataRestrictedRays::
finalize(enum PainterEnums::fill_rule_t f,
         ivec2 pmin_pt, ivec2 pmax_pt)
{
  GlyphRenderDataRestrictedRaysPrivate *d;
  ivec2 sz;

  d = static_cast<GlyphRenderDataRestrictedRaysPrivate*>(m_d);
  FASTUIDRAWassert(d->m_glyph);

  d->m_fill_rule = f;
  d->m_glyph->set_glyph_bounds(pmin_pt, pmax_pt);

  if (d->m_glyph->contours().empty())
    {
      FASTUIDRAWdelete(d->m_glyph);
      d->m_glyph = nullptr;
      d->m_min = ivec2(0, 0);
      d->m_max = ivec2(0, 0);
      d->m_size = ivec2(0, 0);
      return;
    }

  /* Step 0: Scale the value to be in the range [0, 65535] */
  d->m_glyph->translate(-d->m_glyph->bbox().min_point());

  FASTUIDRAWassert(d->m_glyph->bbox().min_point().x() == 0);
  FASTUIDRAWassert(d->m_glyph->bbox().min_point().y() == 0);
  sz = d->m_glyph->bbox().max_point();

  /* if necessary, scale the glyph down (by a power of 2)
   * so that we need only 16-bits to correctly hold a
   * coordinate.
   */
  ivec2 div_scale(1, 1);
  for (int coord = 0; coord < 2; ++coord)
    {
      while (sz[coord] > 65535)
        {
          div_scale[coord] *= 2;
          sz[coord] /= 2;
        }
    }

  if (div_scale != ivec2(1, 1))
    {
      d->m_glyph->scale_down(div_scale);
    }

  /* step 1: create the tree */
  CurveListHierarchy hierarchy(d->m_glyph,
                               d->m_glyph->glyph_bound_min(),
                               d->m_glyph->glyph_bound_max());

  /* step 2: assign tree offsets */
  unsigned int tree_size, total_size;
  tree_size = hierarchy.assign_tree_offsets();

  /* step 3: assign the curve list offsets */
  CurveListCollection curve_lists(tree_size);
  hierarchy.assign_curve_list_offsets(curve_lists);

  /* step 4: assign the offsets to each of the curves */
  total_size = d->m_glyph->assign_curve_offsets(curve_lists.current_offset());

  /* step 5: pack the data */
  d->m_render_data.resize(total_size);
  c_array<generic_data> render_data(make_c_array(d->m_render_data));

  hierarchy.pack_data(render_data);
  curve_lists.pack_data(d->m_glyph, render_data);
  d->m_glyph->pack_data(render_data);

  /* step 7: record the data neeed for shading */
  d->m_min = d->m_glyph->glyph_bound_min();
  d->m_max = d->m_glyph->glyph_bound_max();
  d->m_size = d->m_max - d->m_min;

  FASTUIDRAWdelete(d->m_glyph);
  d->m_glyph = nullptr;
}

enum fastuidraw::return_code
fastuidraw::GlyphRenderDataRestrictedRays::
upload_to_atlas(GlyphAtlasProxy &atlas_proxy,
                GlyphAttribute::Array &attributes) const
{
  GlyphRenderDataRestrictedRaysPrivate *d;
  d = static_cast<GlyphRenderDataRestrictedRaysPrivate*>(m_d);

  FASTUIDRAWassert(!d->m_glyph);

  int data_offset;
  data_offset = atlas_proxy.allocate_data(make_c_array(d->m_render_data));
  if (data_offset == -1)
    {
      return routine_fail;
    }

  attributes.resize(7);
  for (unsigned int c = 0; c < 4; ++c)
    {
      int x, y;

      x = (c & GlyphAttribute::right_corner_mask) ? d->m_size.x() : 0;
      y = (c & GlyphAttribute::top_corner_mask)   ? d->m_size.y() : 0;

      attributes[0].m_data[c] = x;
      attributes[1].m_data[c] = y;
    }
  attributes[2].m_data = uvec4(d->m_size.x());
  attributes[3].m_data = uvec4(d->m_size.y());
  attributes[4].m_data = uvec4(d->m_min.x());
  attributes[5].m_data = uvec4(d->m_min.y());
  attributes[6].m_data = uvec4(data_offset);

  return routine_success;
}
