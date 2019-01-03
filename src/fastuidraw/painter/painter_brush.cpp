/*!
 * \file painter_brush.cpp
 * \brief file painter_brush.cpp
 *
 * Copyright 2016 by Intel.
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

#include <algorithm>
#include <fastuidraw/painter/painter_brush.hpp>

////////////////////////////////////
// fastuidraw::PainterBrush methods
unsigned int
fastuidraw::PainterBrush::
data_size(void) const
{
  unsigned int return_value(0);
  uint32_t pshader = shader();

  return_value += FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(color_data_size);

  if (pshader & image_mask)
    {
      return_value += FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(image_data_size);
    }

  switch(gradient_type())
    {
    case no_gradient_type:
      break;
    case linear_gradient_type:
      return_value += FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(linear_gradient_data_size);
      break;
    case sweep_gradient_type:
      return_value += FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(sweep_gradient_data_size);
      break;
    case radial_gradient_type:
      return_value += FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(radial_gradient_data_size);
      break;
    default:
      FASTUIDRAWassert(!"Invalid gradient_type()");
      break;
    }

  if (pshader & repeat_window_mask)
    {
      return_value += FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(repeat_window_data_size);
    }

  if (pshader & transformation_translation_mask)
    {
      return_value += FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(transformation_translation_data_size);
    }

  if (pshader & transformation_matrix_mask)
    {
      return_value += FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(transformation_matrix_data_size);
    }

  return return_value;
}

void
fastuidraw::PainterBrush::
pack_data(c_array<generic_data> dst) const
{
  unsigned int current(0);
  unsigned int sz;
  c_array<generic_data> sub_dest;
  uint32_t pshader = shader();

  {
    sz = FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(color_data_size);
    sub_dest = dst.sub_array(current, sz);
    current += sz;

    sub_dest[color_red_offset].f = m_data.m_color.x();
    sub_dest[color_green_offset].f = m_data.m_color.y();
    sub_dest[color_blue_offset].f = m_data.m_color.z();
    sub_dest[color_alpha_offset].f = m_data.m_color.w();
  }

  if (pshader & image_mask)
    {
      sz = FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(image_data_size);
      sub_dest = dst.sub_array(current, sz);
      current += sz;

      FASTUIDRAWassert(m_data.m_image);
      uvec3 loc(m_data.m_image->master_index_tile());
      uint32_t slack(m_data.m_image->slack());
      uint32_t lookups(m_data.m_image->number_index_lookups());

      sub_dest[image_size_xy_offset].u =
        pack_bits(image_size_x_bit0, image_size_x_num_bits, m_data.m_image_size.x())
        | pack_bits(image_size_y_bit0, image_size_y_num_bits, m_data.m_image_size.y());

      sub_dest[image_start_xy_offset].u =
        pack_bits(image_size_x_bit0, image_size_x_num_bits, m_data.m_image_start.x())
        | pack_bits(image_size_y_bit0, image_size_y_num_bits, m_data.m_image_start.y());

      if (m_data.m_image->type() == Image::on_atlas)
        {
          sub_dest[image_atlas_location_xyz_offset].u =
            pack_bits(image_atlas_location_x_bit0, image_atlas_location_x_num_bits, loc.x())
            | pack_bits(image_atlas_location_y_bit0, image_atlas_location_y_num_bits, loc.y())
            | pack_bits(image_atlas_location_z_bit0, image_atlas_location_z_num_bits, loc.z());

          sub_dest[image_slack_number_lookups_offset].u =
            pack_bits(image_number_index_lookups_bit0, image_number_index_lookups_num_bits, lookups)
            | pack_bits(image_slack_bit0, image_slack_num_bits, slack);
        }
      else
        {
          uint64_t v, hi, low;

          v = m_data.m_image->bindless_handle();

          hi = uint64_unpack_bits(32, 32, v);
          low = uint64_unpack_bits(0, 32, v);

          sub_dest[image_bindless_handle_hi_offset].u = hi;
          sub_dest[image_bindless_handle_low_offset].u = low;
        }
    }

  enum gradient_type_t tp(gradient_type());
  if (tp != no_gradient_type)
    {
      if (tp == radial_gradient_type)
        {
          sz = FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(radial_gradient_data_size);
        }
      else
        {
          FASTUIDRAWassert(sweep_gradient_data_size == linear_gradient_data_size);
          sz = FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(linear_gradient_data_size);
        }

      sub_dest = dst.sub_array(current, sz);
      current += sz;

      FASTUIDRAWassert(m_data.m_cs);
      FASTUIDRAWassert(m_data.m_cs->texel_location().x() >= 0);
      FASTUIDRAWassert(m_data.m_cs->texel_location().y() >= 0);

      uint32_t x, y;
      x = static_cast<uint32_t>(m_data.m_cs->texel_location().x());
      y = static_cast<uint32_t>(m_data.m_cs->texel_location().y());

      sub_dest[gradient_color_stop_xy_offset].u =
        pack_bits(gradient_color_stop_x_bit0, gradient_color_stop_x_num_bits, x)
        | pack_bits(gradient_color_stop_y_bit0, gradient_color_stop_y_num_bits, y);

      sub_dest[gradient_color_stop_length_offset].u = m_data.m_cs->width();

      sub_dest[gradient_p0_x_offset].f = m_data.m_grad_start.x();
      sub_dest[gradient_p0_y_offset].f = m_data.m_grad_start.y();
      sub_dest[gradient_p1_x_offset].f = m_data.m_grad_end.x();
      sub_dest[gradient_p1_y_offset].f = m_data.m_grad_end.y();

      if (tp == radial_gradient_type)
        {
          sub_dest[gradient_start_radius_offset].f = m_data.m_grad_start_r;
          sub_dest[gradient_end_radius_offset].f = m_data.m_grad_end_r;
        }
    }

  if (pshader & repeat_window_mask)
    {
      sz = FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(repeat_window_data_size);
      sub_dest = dst.sub_array(current, sz);
      current += sz;

      sub_dest[repeat_window_x_offset].f = m_data.m_window_position.x();
      sub_dest[repeat_window_y_offset].f = m_data.m_window_position.y();
      sub_dest[repeat_window_width_offset].f = m_data.m_window_size.x();
      sub_dest[repeat_window_height_offset].f = m_data.m_window_size.y();
    }

  if (pshader & transformation_matrix_mask)
    {
      sz = FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(transformation_matrix_data_size);
      sub_dest = dst.sub_array(current, sz);
      current += sz;

      sub_dest[transformation_matrix_row0_col0_offset].f = m_data.m_transformation_matrix(0, 0);
      sub_dest[transformation_matrix_row0_col1_offset].f = m_data.m_transformation_matrix(0, 1);
      sub_dest[transformation_matrix_row1_col0_offset].f = m_data.m_transformation_matrix(1, 0);
      sub_dest[transformation_matrix_row1_col1_offset].f = m_data.m_transformation_matrix(1, 1);
    }

  if (pshader & transformation_translation_mask)
    {
      sz = FASTUIDRAW_ROUND_UP_MULTIPLE_OF4(transformation_translation_data_size);
      sub_dest = dst.sub_array(current, sz);
      current += sz;

      sub_dest[transformation_translation_x_offset].f = m_data.m_transformation_p.x();
      sub_dest[transformation_translation_y_offset].f = m_data.m_transformation_p.y();
    }

  FASTUIDRAWassert(current == dst.size());
}

fastuidraw::PainterBrush&
fastuidraw::PainterBrush::
sub_image(const reference_counted_ptr<const Image> &im,
          uvec2 xy, uvec2 wh, enum image_filter f,
          unsigned int max_mipmap_level)
{
  uint32_t filter_bits, type_bits, mip_bits;

  filter_bits = (im) ? uint32_t(f) : uint32_t(0);
  type_bits = (im) ? uint32_t(im->type()) : uint32_t(0);
  mip_bits = (im) ?
    uint32_t(t_min(max_mipmap_level, im->number_mipmap_levels())):
    uint32_t(0);
  mip_bits = t_min(mip_bits, FASTUIDRAW_MAX_VALUE_FROM_NUM_BITS(image_mipmap_num_bits));

  m_data.m_image = im;
  m_data.m_image_start = xy;
  m_data.m_image_size = wh;

  m_data.m_shader_raw &= ~image_mask;
  m_data.m_shader_raw |= pack_bits(image_filter_bit0, image_filter_num_bits, filter_bits);

  m_data.m_shader_raw &= ~image_type_mask;
  m_data.m_shader_raw |= pack_bits(image_type_bit0, image_type_num_bits, type_bits);

  m_data.m_shader_raw &= ~image_mipmap_mask;
  m_data.m_shader_raw |= pack_bits(image_mipmap_bit0, image_mipmap_num_bits, mip_bits);

  return *this;
}

fastuidraw::PainterBrush&
fastuidraw::PainterBrush::
image(const reference_counted_ptr<const Image> &im, enum image_filter f,
      unsigned int max_mipmap_level)
{
  uvec2 sz(0, 0);
  if (im)
    {
      sz = uvec2(im->dimensions());
    }
  return sub_image(im, uvec2(0,0), sz, f, max_mipmap_level);
}

uint32_t
fastuidraw::PainterBrush::
shader(void) const
{
  uint32_t return_value;

  FASTUIDRAWstatic_assert(number_shader_bits <= 32u);

  return_value = m_data.m_shader_raw;
  if (!m_data.m_image && !m_data.m_cs)
    {
      /* lacking an image or gradient means the brush does
       * nothing and so all bits should be down.
       */
      return_value = 0;
    }
  return return_value;
}

void
fastuidraw::PainterBrush::
reset(void)
{
  color(1.0, 1.0, 1.0, 1.0);
  m_data.m_shader_raw = 0u;
  m_data.m_image = nullptr;
  m_data.m_cs = nullptr;
}

enum fastuidraw::PainterBrush::image_filter
fastuidraw::PainterBrush::
filter_for_image(const reference_counted_ptr<const Image> &im,
                 enum image_filter f)
{
  return im ?
    static_cast<enum image_filter>(t_min(im->slack() + 1,
                                         static_cast<unsigned int>(f))) :
    image_filter_nearest;
}

bool
fastuidraw::PainterBrush::
filter_suitable_for_image(const reference_counted_ptr<const Image> &im,
                          enum image_filter f)
{
  FASTUIDRAWassert(f >= image_filter_nearest);
  FASTUIDRAWassert(f <= image_filter_cubic);
  return im && im->slack() >= static_cast<unsigned int>(f) - 1;
}

int
fastuidraw::PainterBrush::
slack_requirement(enum image_filter f)
{
  FASTUIDRAWassert(f >= image_filter_nearest);
  FASTUIDRAWassert(f <= image_filter_cubic);
  return f - 1;
}
