/*
 * glade-design-layout.c
 *
 * Copyright (C) 2006-2007 Vincent Geddes
 *                    2011 Juan Pablo Ugarte
 *
 * Authors:
 *   Vincent Geddes <vgeddes@gnome.org>
 *   Juan Pablo Ugarte <juanpablougarte@gmail.com>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public 
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"

#include "glade.h"
#include "glade-design-layout.h"
#include "glade-design-private.h"
#include "glade-accumulators.h"
#include "glade-marshallers.h"

#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#define GLADE_DESIGN_LAYOUT_GET_PRIVATE(object) (G_TYPE_INSTANCE_GET_PRIVATE ((object),  \
						 GLADE_TYPE_DESIGN_LAYOUT,               \
						 GladeDesignLayoutPrivate))
#define GLADE_DESIGN_LAYOUT_PRIVATE(object) (((GladeDesignLayout*)object)->priv)

#define OUTLINE_WIDTH     4
#define PADDING           12

#define MARGIN_STEP       6

typedef enum
{
  ACTIVITY_NONE,
  ACTIVITY_RESIZE_WIDTH,
  ACTIVITY_RESIZE_HEIGHT,
  ACTIVITY_RESIZE_WIDTH_AND_HEIGHT,
  ACTIVITY_ALIGNMENTS,
  ACTIVITY_MARGINS,
  ACTIVITY_MARGINS_VERTICAL, /* These activities are only used to set the cursor */
  ACTIVITY_MARGINS_HORIZONTAL,
  ACTIVITY_MARGINS_TOP_LEFT,
  ACTIVITY_MARGINS_TOP_RIGHT,
  ACTIVITY_MARGINS_BOTTOM_LEFT,
  ACTIVITY_MARGINS_BOTTOM_RIGHT,
  N_ACTIVITY
} Activity;


typedef enum
{
  MARGIN_TOP    = 1 << 0,
  MARGIN_BOTTOM = 1 << 1,
  MARGIN_LEFT   = 1 << 2,
  MARGIN_RIGHT  = 1 << 3
} Margins;

struct _GladeDesignLayoutPrivate
{
  GdkWindow *window, *offscreen_window;

  gint child_offset;
  GdkRectangle east, south, south_east;
  GdkCursor *cursor;            /* Current cursor */
  GdkCursor *cursors[N_ACTIVITY];

  gint current_width, current_height;
  PangoLayout *widget_name;
  gint layout_width;

  GtkStyleContext *default_context;

  /* Colors */
  GdkRGBA fg_color;
  GdkRGBA frame_color[2];
  GdkRGBA frame_color_active[2];

  /* Margin edit mode */
  GtkWidget *selection;
  gint top, bottom, left, right;
  gint m_dy, m_dx;
  gint max_width, max_height;
  Margins margin;
  GtkAlign valign, halign;
  Margins node_over;

  /* state machine */
  Activity activity;            /* the current activity */
  gint dx;                      /* child.width - event.pointer.x   */
  gint dy;                      /* child.height - event.pointer.y  */
  gint new_width;               /* user's new requested width */
  gint new_height;              /* user's new requested height */

  /* Drag & Drop */
  GtkWidget *drag_source;
  GtkWidget *drag_icon;
  gint drag_x, drag_y;

  /* Properties */
  GladeDesignView *view;
  GladeProject *project;
};

enum
{
  PROP_0,
  PROP_DESIGN_VIEW
};

G_DEFINE_TYPE (GladeDesignLayout, glade_design_layout, GTK_TYPE_BIN)

#define RECTANGLE_POINT_IN(rect,x,y) (x >= rect.x && x <= (rect.x + rect.width) && y >= rect.y && y <= (rect.y + rect.height))

static Margins
gdl_get_margins_from_pointer (GtkWidget *child, GtkWidget *widget, gint x, gint y)
{
  gint width, height, xx, yy, top, bottom, left, right;
  Margins margin = 0;
  GdkRectangle rec;
  
  width = gtk_widget_get_allocated_width (widget);
  height = gtk_widget_get_allocated_height (widget);

  gtk_widget_translate_coordinates (widget, child, 0, 0, &xx, &yy);
  
  top = gtk_widget_get_margin_top (widget);
  bottom = gtk_widget_get_margin_bottom (widget);
  left = gtk_widget_get_margin_left (widget);
  right = gtk_widget_get_margin_right (widget);

  rec.x = xx - left - OUTLINE_WIDTH;
  rec.y = yy - top - OUTLINE_WIDTH;
  rec.width = width + left + right + (OUTLINE_WIDTH * 2);
  rec.height = height + top + bottom + (OUTLINE_WIDTH * 2);

  if (RECTANGLE_POINT_IN (rec, x, y))
    {      
      if (y <= yy + OUTLINE_WIDTH) margin |= MARGIN_TOP;
      else if (y >= yy + height - OUTLINE_WIDTH) margin |= MARGIN_BOTTOM;
      
      if (x <= xx + OUTLINE_WIDTH) margin |= MARGIN_LEFT;
      else if (x >= xx + width - OUTLINE_WIDTH) margin |= MARGIN_RIGHT;
    }

  return margin;
}

static Activity
gdl_get_activity_from_pointer (GladeDesignLayout *layout, gint x, gint y)
{
  GladeDesignLayoutPrivate *priv = GLADE_DESIGN_LAYOUT_GET_PRIVATE (layout);
  
  if (priv->selection)
    {
      priv->margin = gdl_get_margins_from_pointer (GTK_WIDGET (layout),
                                                   priv->selection,
                                                   x, y);
      
      if (priv->margin)
        {
          GladePointerMode mode = glade_project_get_pointer_mode (priv->project);
          return (mode == GLADE_POINTER_ALIGN_EDIT) ? ACTIVITY_ALIGNMENTS : ACTIVITY_MARGINS;
        }
    }
  
  if (RECTANGLE_POINT_IN (priv->south_east, x, y)) return ACTIVITY_RESIZE_WIDTH_AND_HEIGHT;

  if (RECTANGLE_POINT_IN (priv->east, x, y)) return ACTIVITY_RESIZE_WIDTH;

  if (RECTANGLE_POINT_IN (priv->south, x, y)) return ACTIVITY_RESIZE_HEIGHT;

  return ACTIVITY_NONE;
}

static void
gdl_set_cursor (GladeDesignLayoutPrivate *priv, GdkCursor *cursor)
{
  if (cursor != priv->cursor)
    {
      priv->cursor = cursor;
      gdk_window_set_cursor (priv->window, cursor);
    }
}

static Activity
gdl_margin_get_activity (Margins margin)
{
  if (margin & MARGIN_TOP)
    {
      if (margin & MARGIN_LEFT)
        return ACTIVITY_MARGINS_TOP_LEFT;
      else if (margin & MARGIN_RIGHT)
        return ACTIVITY_MARGINS_TOP_RIGHT;
      else
        return ACTIVITY_MARGINS_VERTICAL;
    }
  else if (margin & MARGIN_BOTTOM)
    {
      if (margin & MARGIN_LEFT)
        return ACTIVITY_MARGINS_BOTTOM_LEFT;
      else if (margin & MARGIN_RIGHT)
        return ACTIVITY_MARGINS_BOTTOM_RIGHT;
      else
        return ACTIVITY_MARGINS_VERTICAL;
    }
  else if (margin & MARGIN_LEFT || margin & MARGIN_RIGHT)
    return ACTIVITY_MARGINS_HORIZONTAL;

  return ACTIVITY_NONE;
}

static gboolean
glade_design_layout_leave_notify_event (GtkWidget *widget, GdkEventCrossing *ev)
{
  GtkWidget *child;
  GladeDesignLayoutPrivate *priv;

  if ((child = gtk_bin_get_child (GTK_BIN (widget))) == NULL ||
      ev->window != gtk_widget_get_window (widget))
    return FALSE;

  priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);

  if (priv->activity == ACTIVITY_NONE)
    gdl_set_cursor (priv, NULL);

  return FALSE;
}

static void
gdl_update_max_margins (GladeDesignLayout *layout,
                        GtkWidget *child,
                        gint width, gint height)
{
  GladeDesignLayoutPrivate *priv = layout->priv;
  gint parent_w, parent_h, layout_w, layout_h;
  gint top, bottom, left, right;
  GtkRequisition req;
  
  gtk_widget_get_preferred_size (child, &req, NULL);

  top = gtk_widget_get_margin_top (priv->selection);
  bottom = gtk_widget_get_margin_bottom (priv->selection);
  left = gtk_widget_get_margin_left (priv->selection);
  right = gtk_widget_get_margin_right (priv->selection);
  
  priv->max_width = width - (req.width - left - right);

  parent_w = gtk_widget_get_allocated_width (GTK_WIDGET (priv->view));
  layout_w = gtk_widget_get_allocated_width (GTK_WIDGET (layout));

  if (parent_w > layout_w)
    priv->max_width += parent_w - layout_w - (PADDING - OUTLINE_WIDTH);
  
  priv->max_height = height - (req.height - top - bottom) ;

  parent_h = gtk_widget_get_allocated_height (GTK_WIDGET (priv->view));
  layout_h = gtk_widget_get_allocated_height (GTK_WIDGET (layout));
  if (parent_h > layout_h)
    priv->max_height += parent_h - layout_h - (PADDING - OUTLINE_WIDTH);
}

static void
glade_design_layout_update_child (GladeDesignLayout *layout,
                                  GtkWidget         *child,
                                  GtkAllocation     *allocation)
{
  GladeWidget *gchild;

  /* Update GladeWidget metadata */
  gchild = glade_widget_get_from_gobject (child);
  g_object_set (gchild,
                "toplevel-width", allocation->width,
                "toplevel-height", allocation->height, NULL);

  if (layout->priv->selection)
    gdl_update_max_margins (layout, child, allocation->width, allocation->height);

  gtk_widget_queue_resize (GTK_WIDGET (layout));
}

static inline void
gdl_alignments_invalidate (GdkWindow *window,
                           GtkWidget *parent,
                           GtkWidget *selection,
                           Margins nodes)
{
  cairo_region_t *region = cairo_region_create ();
  cairo_rectangle_int_t rect = {0, 0, 16, 16};
  gint x1, x2, x3, y1, y2, y3;
  GtkAllocation alloc;
  gint x, y, w, h;

  gtk_widget_get_allocation (selection, &alloc);

  w = alloc.width;
  h = alloc.height;

  gtk_widget_translate_coordinates (selection, parent, 0, 0, &x, &y);

  x1 = x - gtk_widget_get_margin_left (selection);
  x2 = x + w/2;
  x3 = x + w + gtk_widget_get_margin_right (selection);
  y1 = y - gtk_widget_get_margin_top (selection);
  y2 = y + h/2;
  y3 = y + h + gtk_widget_get_margin_bottom (selection);

  /* Only invalidate node area */
  if (nodes & MARGIN_TOP)
    {
      rect.x = x2 - 5;
      rect.y = y1 - 10;
      cairo_region_union_rectangle (region, &rect);
    }
  if (nodes & MARGIN_BOTTOM)
    {
      rect.x = x2 - 8;
      rect.y = y3 - 13;
      cairo_region_union_rectangle (region, &rect);
    }

  rect.y = y2 - 10;
  if (nodes & MARGIN_LEFT)
    {
      rect.x = x1 - 8;
      cairo_region_union_rectangle (region, &rect);
    }
  if (nodes & MARGIN_RIGHT)
    {
      rect.x = x3 - 5;
      cairo_region_union_rectangle (region, &rect);
    }

  gdk_window_invalidate_region (window, region, FALSE);

  cairo_region_destroy (region);
}

static gboolean
glade_design_layout_motion_notify_event (GtkWidget *widget, GdkEventMotion *ev)
{
  GladeDesignLayoutPrivate *priv;
  GtkAllocation allocation;
  GtkWidget *child;
  gint x, y;

  if ((child = gtk_bin_get_child (GTK_BIN (widget))) == NULL)
    return FALSE;

  priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);

  x = ev->x;
  y = ev->y;

  if (ev->state & GDK_BUTTON1_MASK && priv->drag_source &&
      gtk_drag_check_threshold (priv->drag_source, priv->drag_x, priv->drag_y, x, y))
    {
      static GtkTargetList *target = NULL;

      if (target == NULL)
        target = gtk_target_list_new (_glade_design_layout_get_dnd_target (), 1);

      gtk_drag_begin (widget, target, GDK_ACTION_COPY, 1, (GdkEvent*)ev);
      return TRUE;
    }

  gtk_widget_get_allocation (child, &allocation);

  allocation.x += priv->child_offset;
  allocation.y += priv->child_offset;

  switch (priv->activity)
    {
      case ACTIVITY_RESIZE_WIDTH:
        allocation.width = MAX (0, x - priv->dx - PADDING - OUTLINE_WIDTH);
      break;
      case ACTIVITY_RESIZE_HEIGHT:
        allocation.height = MAX (0, y - priv->dy - PADDING - OUTLINE_WIDTH);
      break;
      case ACTIVITY_RESIZE_WIDTH_AND_HEIGHT:
        allocation.height = MAX (0, y - priv->dy - PADDING - OUTLINE_WIDTH);
        allocation.width = MAX (0, x - priv->dx - PADDING - OUTLINE_WIDTH);
      break;
      case ACTIVITY_MARGINS:
        {
          gboolean shift = ev->state & GDK_SHIFT_MASK;
          gboolean snap = ev->state & GDK_CONTROL_MASK;
          GtkWidget *selection = priv->selection;
          Margins margin = priv->margin;

          if (margin & MARGIN_TOP)
            {
              gint max_height = (shift) ? priv->max_height/2 : priv->max_height -
                gtk_widget_get_margin_bottom (selection);
              gint val = MAX (0, MIN (priv->m_dy - y, max_height));
              
              if (snap) val = (val/MARGIN_STEP)*MARGIN_STEP;
              gtk_widget_set_margin_top (selection, val);
              if (shift) gtk_widget_set_margin_bottom (selection, val);
            }
          else if (margin & MARGIN_BOTTOM)
            {
              gint max_height = (shift) ? priv->max_height/2 : priv->max_height -
                gtk_widget_get_margin_top (selection);
              gint val = MAX (0, MIN (y - priv->m_dy, max_height));
              
              if (snap) val = (val/MARGIN_STEP)*MARGIN_STEP;
              gtk_widget_set_margin_bottom (selection, val);
              if (shift) gtk_widget_set_margin_top (selection, val);
            }

          if (margin & MARGIN_LEFT)
            {
              gint max_width = (shift) ? priv->max_width/2 : priv->max_width -
                gtk_widget_get_margin_right (selection);
              gint val = MAX (0, MIN (priv->m_dx - x, max_width));
              
              if (snap) val = (val/MARGIN_STEP)*MARGIN_STEP;
              gtk_widget_set_margin_left (selection, val);
              if (shift) gtk_widget_set_margin_right (selection, val);
            }
          else if (margin & MARGIN_RIGHT)
            {
              gint max_width = (shift) ? priv->max_width/2 : priv->max_width -
                gtk_widget_get_margin_left (selection);
              gint val = MAX (0, MIN (x - priv->m_dx, max_width));
              
              if (snap) val = (val/MARGIN_STEP)*MARGIN_STEP;
              gtk_widget_set_margin_right (selection, val);
              if (shift) gtk_widget_set_margin_left (selection, val);
            }
        }
      break;
      default:
        {
          Activity activity = gdl_get_activity_from_pointer (GLADE_DESIGN_LAYOUT (widget), x, y);

          if (priv->node_over != priv->margin && (activity == ACTIVITY_ALIGNMENTS ||
              glade_project_get_pointer_mode (priv->project) == GLADE_POINTER_ALIGN_EDIT))
            {
              if (priv->selection)
                gdl_alignments_invalidate (priv->window, widget, priv->selection,
                                           priv->node_over | priv->margin);
              else
                gdk_window_invalidate_rect (priv->window, NULL, FALSE);

              priv->node_over = priv->margin;
            }
           
          if (activity == ACTIVITY_MARGINS)
            activity = gdl_margin_get_activity (priv->margin);

          /* Only set the cursor if changed */
          gdl_set_cursor (priv, priv->cursors[activity]);
          return TRUE;
        }
      break;
    }

  glade_design_layout_update_child (GLADE_DESIGN_LAYOUT (widget), child, &allocation);
  return FALSE;
}

typedef struct
{
  GtkWidget *toplevel;
  gint x;
  gint y;
  GtkWidget *placeholder;
  GladeWidget *gwidget;
} GladeFindInContainerData;

static void
glade_design_layout_find_inside_container (GtkWidget                *widget,
                                           GladeFindInContainerData *data)
{
  gint x, y, w, h;

  if (data->gwidget || !gtk_widget_get_mapped (widget))
    return;

  gtk_widget_translate_coordinates (data->toplevel, widget, data->x, data->y,
                                    &x, &y);
  
  /* Margins are not part of the widget allocation */
  w = gtk_widget_get_allocated_width (widget) + gtk_widget_get_margin_right (widget);
  h = gtk_widget_get_allocated_height (widget) + gtk_widget_get_margin_bottom (widget);

  if (x >= (0 - gtk_widget_get_margin_left (widget)) && x < w &&
      y >= (0 - gtk_widget_get_margin_top (widget)) && y < h)
    {
      if (GLADE_IS_PLACEHOLDER (widget))
        data->placeholder = widget;
      else
        {
          if (GTK_IS_CONTAINER (widget))
            gtk_container_forall (GTK_CONTAINER (widget), (GtkCallback)
                                  glade_design_layout_find_inside_container,
                                  data);

          if (!data->gwidget)
            data->gwidget = glade_widget_get_from_gobject (widget);
        }
    }
}

static gboolean
glade_project_is_toplevel_active (GladeProject *project, GtkWidget *toplevel)
{
  GList *l;

  for (l = glade_project_selection_get (project); l; l = g_list_next (l))
    {
      if (GTK_IS_WIDGET (l->data) && 
	  gtk_widget_is_ancestor (l->data, toplevel)) return TRUE;
    }

  return FALSE;
}

static void
gdl_edit_mode_set_selection (GladeDesignLayout *layout,
                             GladePointerMode mode,
                             GtkWidget *selection)
{
  GladeDesignLayoutPrivate *priv = layout->priv;

  if ((selection && GTK_IS_WIDGET (selection) == FALSE) ||
      gtk_bin_get_child (GTK_BIN (layout)) == selection) selection = NULL;
  
  if (priv->selection == selection) return;

  priv->selection = selection;

  if (selection)
    {
      if (mode == GLADE_POINTER_MARGIN_EDIT)
        {
          GtkWidget *child = gtk_bin_get_child (GTK_BIN (layout));

          /* Save initital margins to know which one where edited */
          priv->top = gtk_widget_get_margin_top (selection);
          priv->bottom = gtk_widget_get_margin_bottom (selection);
          priv->left = gtk_widget_get_margin_left (selection);
          priv->right = gtk_widget_get_margin_right (selection);

          gdl_update_max_margins (layout, child,
                                  gtk_widget_get_allocated_width (child),
                                  gtk_widget_get_allocated_height (child));
        }
      else if (mode == GLADE_POINTER_ALIGN_EDIT)
        {
          priv->valign = gtk_widget_get_valign (selection);
          priv->halign = gtk_widget_get_halign (selection);
        }

      gdk_window_invalidate_rect (priv->window, NULL, FALSE);
    }
  else
    {
      gdl_set_cursor (priv, NULL);
    }

  glade_project_set_pointer_mode (priv->project, mode);
}

static gboolean
glade_design_layout_button_press_event (GtkWidget *widget, GdkEventButton *ev)
{
  GladeDesignLayoutPrivate *priv;
  GtkAllocation child_allocation;
  Activity activity;
  GtkWidget *child;
  gint x, y;

  if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS ||
      (child = gtk_bin_get_child (GTK_BIN (widget))) == NULL)
    return FALSE;

  priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);

  x = ev->x;
  y = ev->y;

  priv->activity = activity = gdl_get_activity_from_pointer (GLADE_DESIGN_LAYOUT (widget), x, y);

  /* Check if we are in margin edit mode */
  if (priv->selection)
    {
      GtkWidget *selection = priv->selection;

      switch (activity)
        {
          case ACTIVITY_NONE:
            gdl_edit_mode_set_selection (GLADE_DESIGN_LAYOUT (widget), GLADE_POINTER_SELECT, NULL);
            return FALSE;
          break;
          case ACTIVITY_ALIGNMENTS:
            {
              gboolean top, bottom, left, right;
              Margins node = priv->margin;
              GtkAlign valign, halign;
              GladeWidget *gwidget;

              valign = gtk_widget_get_valign (selection);
              halign = gtk_widget_get_halign (selection);

              if (valign == GTK_ALIGN_FILL)
                top = bottom = TRUE;
              else
                {
                  top = (valign == GTK_ALIGN_START);
                  bottom = (valign == GTK_ALIGN_END);
                }

              if (halign == GTK_ALIGN_FILL)
                left = right = TRUE;
              else
                {
                  left = (halign == GTK_ALIGN_START);
                  right = (halign == GTK_ALIGN_END);
                }

              if (node & MARGIN_TOP)
                valign = (top) ? ((bottom) ? GTK_ALIGN_END : GTK_ALIGN_CENTER) : 
                                 ((bottom) ? GTK_ALIGN_FILL : GTK_ALIGN_START);
              else if (node & MARGIN_BOTTOM)
                valign = (bottom) ? ((top) ? GTK_ALIGN_START : GTK_ALIGN_CENTER) :
                                    ((top) ? GTK_ALIGN_FILL : GTK_ALIGN_END);

              if (node & MARGIN_LEFT)
                halign = (left) ? ((right) ? GTK_ALIGN_END : GTK_ALIGN_CENTER) :
                                  ((right) ? GTK_ALIGN_FILL : GTK_ALIGN_START);
              else if (node & MARGIN_RIGHT)
                halign = (right) ? ((left) ? GTK_ALIGN_START : GTK_ALIGN_CENTER) :
                                   ((left) ? GTK_ALIGN_FILL : GTK_ALIGN_END);

              if ((gwidget = glade_widget_get_from_gobject (selection)))
                {
                  GladeProperty *property;

                  glade_command_push_group (_("Editing alignments of %s"),
                                            glade_widget_get_name (gwidget));

                  if (gtk_widget_get_valign (selection) != valign)
                    {
                      if ((property = glade_widget_get_property (gwidget, "valign")))
                        glade_command_set_property (property, valign);
                    }
                  if (gtk_widget_get_halign (selection) != halign)
                    {
                      if ((property = glade_widget_get_property (gwidget, "halign")))
                        glade_command_set_property (property, halign);
                    }
                  glade_command_pop_group ();
                }
            }
          break;
          case ACTIVITY_MARGINS:
            priv->m_dx = x + ((priv->margin & MARGIN_LEFT) ? 
                              gtk_widget_get_margin_left (selection) :
                                gtk_widget_get_margin_right (selection) * -1);
            priv->m_dy = y + ((priv->margin & MARGIN_TOP) ?
                              gtk_widget_get_margin_top (selection) :
                                gtk_widget_get_margin_bottom (selection) * -1);

            gdl_set_cursor (priv, priv->cursors[gdl_margin_get_activity (priv->margin)]);
            return FALSE;
          break;
          default:
            gdl_set_cursor (priv, priv->cursors[priv->activity]);
          break;
        }
    }

  gtk_widget_get_allocation (child, &child_allocation);

  priv->dx = x - (child_allocation.x + child_allocation.width + priv->child_offset);
  priv->dy = y - (child_allocation.y + child_allocation.height + priv->child_offset);

  if (activity != ACTIVITY_NONE &&
      !glade_project_is_toplevel_active (priv->project, child))
    {
      _glade_design_view_freeze (priv->view);
      glade_project_selection_set (priv->project, G_OBJECT (child), TRUE);
      _glade_design_view_thaw (priv->view);
    }

  return FALSE;
}

static gboolean
glade_design_layout_button_release_event (GtkWidget *widget,
                                          GdkEventButton *ev)
{
  GladeDesignLayoutPrivate *priv;
  GtkWidget *child;

  if ((child = gtk_bin_get_child (GTK_BIN (widget))) == NULL)
    return FALSE;

  priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);

  /* Check if margins where edited and execute corresponding glade command */
  if (priv->selection && priv->activity == ACTIVITY_MARGINS)
    {
      GladeWidget *gwidget = glade_widget_get_from_gobject (priv->selection);
      gint top, bottom, left, right;
      GladeProperty *property;

      top = gtk_widget_get_margin_top (priv->selection);
      bottom = gtk_widget_get_margin_bottom (priv->selection);
      left = gtk_widget_get_margin_left (priv->selection);
      right = gtk_widget_get_margin_right (priv->selection);

      glade_command_push_group (_("Editing margins of %s"),
                                glade_widget_get_name (gwidget));
      if (priv->top != top)
        {
          if ((property = glade_widget_get_property (gwidget, "margin-top")))
            glade_command_set_property (property, top);
        }
      if (priv->bottom != bottom)
        {
          if ((property = glade_widget_get_property (gwidget, "margin-bottom")))
            glade_command_set_property (property, bottom);
        }
      if (priv->left != left)
        {
          if ((property = glade_widget_get_property (gwidget, "margin-left")))
            glade_command_set_property (property, left);
        }
      if (priv->right != right)
        {
          if ((property = glade_widget_get_property (gwidget, "margin-right")))
            glade_command_set_property (property, right);
        }

      glade_command_pop_group ();
    }
  else if (priv->activity == ACTIVITY_ALIGNMENTS)
    {
      priv->node_over = 0;
      gdk_window_invalidate_rect (priv->window, NULL, FALSE);
    }
  
  priv->activity = ACTIVITY_NONE;
  gdl_set_cursor (priv, NULL);

  return FALSE;
}

static void
glade_design_layout_get_preferred_height (GtkWidget *widget,
                                          gint *minimum, gint *natural)
{
  GladeDesignLayoutPrivate *priv;
  GtkWidget *child;
  GladeWidget *gchild;
  gint child_height = 0;
  guint border_width = 0;

  priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);

  *minimum = 0;

  child = gtk_bin_get_child (GTK_BIN (widget));

  if (child && gtk_widget_get_visible (child))
    {
      GtkRequisition req;
      gint height;

      gchild = glade_widget_get_from_gobject (child);
      g_assert (gchild);

      gtk_widget_get_preferred_size (child, &req, NULL);

      g_object_get (gchild, "toplevel-height", &child_height, NULL);

      child_height = MAX (child_height, req.height);

      if (priv->widget_name)
        pango_layout_get_pixel_size (priv->widget_name, NULL, &height);
      else
        height = PADDING;

      *minimum = MAX (*minimum, PADDING + 2.5*OUTLINE_WIDTH + height + child_height);
    }

  border_width = gtk_container_get_border_width (GTK_CONTAINER (widget));
  *minimum += border_width * 2;
  *natural = *minimum;
}

static void
glade_design_layout_get_preferred_width (GtkWidget *widget,
                                         gint *minimum, gint *natural)
{
  GtkWidget *child;
  GladeWidget *gchild;
  gint child_width = 0;
  guint border_width = 0;

  *minimum = 0;

  child = gtk_bin_get_child (GTK_BIN (widget));

  if (child && gtk_widget_get_visible (child))
    {
      GtkRequisition req;
      
      gchild = glade_widget_get_from_gobject (child);
      g_assert (gchild);

      gtk_widget_get_preferred_size (child, &req, NULL);

      g_object_get (gchild, "toplevel-width", &child_width, NULL);

      child_width = MAX (child_width, req.width);

      *minimum = MAX (*minimum, 2*PADDING + 2*OUTLINE_WIDTH + child_width);
    }

  border_width = gtk_container_get_border_width (GTK_CONTAINER (widget));
  *minimum += border_width * 2;
  *natural = *minimum;
}

static void
glade_design_layout_get_preferred_width_for_height (GtkWidget       *widget,
                                                    gint             height,
                                                    gint            *minimum_width,
                                                    gint            *natural_width)
{
  glade_design_layout_get_preferred_width (widget, minimum_width, natural_width);
}

static void
glade_design_layout_get_preferred_height_for_width (GtkWidget       *widget,
                                                    gint             width,
                                                    gint            *minimum_height,
                                                    gint            *natural_height)
{
  glade_design_layout_get_preferred_height (widget, minimum_height, natural_height);
}

static void
update_rectangles (GladeDesignLayoutPrivate *priv, GtkAllocation *alloc)
{
  GdkRectangle *rect = &priv->south_east;
  gint width, height;

  /* Update rectangles used to resize the children */
  priv->east.x = alloc->width + priv->child_offset;
  priv->east.y = priv->child_offset;
  priv->east.height = alloc->height;

  priv->south.x = priv->child_offset;
  priv->south.y = alloc->height + priv->child_offset;
  priv->south.width = alloc->width;
  
  /* Update south east rectangle width */
  pango_layout_get_pixel_size (priv->widget_name, &width, &height);
  priv->layout_width = width + (OUTLINE_WIDTH*2);
  width = MIN (alloc->width, width);

  rect->x = alloc->x + priv->child_offset + alloc->width - width - OUTLINE_WIDTH/2;
  rect->y = alloc->y + priv->child_offset + alloc->height + OUTLINE_WIDTH/2;
  rect->width = width + (OUTLINE_WIDTH*2);
  rect->height = height + OUTLINE_WIDTH;

  /* Update south rectangle width */
  priv->south.width = rect->x - priv->south.x;
}

static void
glade_design_layout_size_allocate (GtkWidget *widget,
                                   GtkAllocation *allocation)
{
  GtkWidget *child;

  gtk_widget_set_allocation (widget, allocation);
    
  if (gtk_widget_get_realized (widget))
  {
    gdk_window_move_resize (gtk_widget_get_window (widget),
                            allocation->x, allocation->y,
                            allocation->width, allocation->height);
  }

  child = gtk_bin_get_child (GTK_BIN (widget));

  if (child && gtk_widget_get_visible (child))
    {
      GladeDesignLayoutPrivate *priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);
      GtkAllocation alloc;
      gint height, offset;

      offset = gtk_container_get_border_width (GTK_CONTAINER (widget)) + PADDING + OUTLINE_WIDTH;
      priv->child_offset = offset;

      if (priv->widget_name)
        pango_layout_get_pixel_size (priv->widget_name, NULL, &height);
      else
        height = PADDING;
      
      alloc.x = alloc.y = 0;
      priv->current_width = alloc.width = allocation->width - (offset * 2);
      priv->current_height = alloc.height = allocation->height - (offset + OUTLINE_WIDTH * 1.5 + height);
      
      if (gtk_widget_get_realized (widget))
        gdk_window_move_resize (priv->offscreen_window,
                                0, 0, alloc.width, alloc.height);

      gtk_widget_size_allocate (child, &alloc);
      update_rectangles (priv, &alloc);
    }
}

static void
on_glade_widget_name_notify (GObject *gobject, GParamSpec *pspec, GladeDesignLayout *layout) 
{
  GladeDesignLayoutPrivate *priv = layout->priv;
  
  pango_layout_set_text (priv->widget_name, glade_widget_get_name (GLADE_WIDGET (gobject)), -1);
  gtk_widget_queue_resize (GTK_WIDGET (layout));
}

static void
glade_design_layout_add (GtkContainer *container, GtkWidget *widget)
{
  GladeDesignLayout *layout = GLADE_DESIGN_LAYOUT (container);
  GladeDesignLayoutPrivate *priv = layout->priv;
  GladeWidget *gchild;

  layout->priv->current_width = 0;
  layout->priv->current_height = 0;

  gtk_widget_set_parent_window (widget, priv->offscreen_window);

  GTK_CONTAINER_CLASS (glade_design_layout_parent_class)->add (container,
                                                               widget);

  if ((gchild = glade_widget_get_from_gobject (G_OBJECT (widget))))
    {
      on_glade_widget_name_notify (G_OBJECT (gchild), NULL, layout);
      g_signal_connect (gchild, "notify::name", G_CALLBACK (on_glade_widget_name_notify), layout);
    }
    
  gtk_widget_queue_draw (GTK_WIDGET (container)); 
}

static void
glade_design_layout_remove (GtkContainer *container, GtkWidget *widget)
{
  GladeWidget *gchild;

  if ((gchild = glade_widget_get_from_gobject (G_OBJECT (widget))))
    g_signal_handlers_disconnect_by_func (gchild, on_glade_widget_name_notify,
                                          GLADE_DESIGN_LAYOUT (container));

  GTK_CONTAINER_CLASS (glade_design_layout_parent_class)->remove (container, widget);
  gtk_widget_queue_draw (GTK_WIDGET (container));
}

static gboolean
glade_design_layout_damage (GtkWidget *widget, GdkEventExpose *event)
{
  gdk_window_invalidate_rect (gtk_widget_get_window (widget), NULL, TRUE);
  return TRUE;
}

static inline void
draw_frame (cairo_t *cr, GladeDesignLayoutPrivate *priv, gboolean selected,
            int x, int y, int w, int h)
{
  cairo_save (cr);

  cairo_set_line_width (cr, OUTLINE_WIDTH);

  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

  gdk_cairo_set_source_rgba (cr, (selected) ? &priv->frame_color_active[0] :
                             &priv->frame_color[0]);

  /* rectangle */
  cairo_rectangle (cr, x, y, w, h);
  cairo_stroke (cr);

  if (priv->widget_name)
    {
      GdkRGBA *color = (selected) ? &priv->frame_color_active[1] : &priv->frame_color[1];
      GdkRectangle *rect = &priv->south_east;
      cairo_pattern_t *pattern;
      gdouble xx, yy;
      
      xx = rect->x + rect->width;
      yy = rect->y + rect->height;

      /* Draw tab */
      cairo_move_to (cr, rect->x, rect->y);
      cairo_line_to (cr, xx, rect->y);
      cairo_line_to (cr, xx, yy-8);
      cairo_curve_to (cr, xx, yy, xx, yy, xx-8, yy);
      cairo_line_to (cr, rect->x+8, yy);
      cairo_curve_to (cr, rect->x, yy, rect->x, yy, rect->x, yy-8);
      cairo_close_path (cr);
      cairo_fill (cr);

      /* Draw widget name */
      if (rect->width < priv->layout_width)
        {
          gdouble r = color->red, g = color->green, b = color->blue;
          
          pattern = cairo_pattern_create_linear (xx-16-OUTLINE_WIDTH, 0,
                                                 xx-OUTLINE_WIDTH, 0);
          cairo_pattern_add_color_stop_rgba (pattern, 0, r, g, b, 1);
          cairo_pattern_add_color_stop_rgba (pattern, 1, r, g, b, 0);
          cairo_set_source (cr, pattern);
        }
      else
        {
          pattern = NULL;
          gdk_cairo_set_source_rgba (cr, color);
        }

      cairo_move_to (cr, rect->x + OUTLINE_WIDTH, rect->y + OUTLINE_WIDTH);
      pango_cairo_show_layout (cr, priv->widget_name);

      if (pattern) cairo_pattern_destroy (pattern);
    }

  cairo_restore (cr);
}

static void
draw_margin_selection (cairo_t *cr,
                       gint x1, gint x2, gint x3, gint x4, 
                       gint y1, gint y2, gint y3, gint y4,
                       gdouble r, gdouble g, gdouble b,
                       gint x5, gint y5)
{
  cairo_pattern_t *gradient = cairo_pattern_create_linear (x1, y1, x5, y5);

  cairo_pattern_add_color_stop_rgba (gradient, 0, r+.24, g+.24, b+.24, .08);
  cairo_pattern_add_color_stop_rgba (gradient, 1, r, g, b, .16);
  
  cairo_set_source (cr, gradient);
  
  cairo_move_to (cr, x1, y1);
  cairo_line_to (cr, x2, y2);
  cairo_line_to (cr, x3, y3);
  cairo_line_to (cr, x4, y4);
  cairo_close_path (cr);
  cairo_fill (cr);

  cairo_pattern_destroy (gradient);
}

static inline void
draw_selection (cairo_t *cr,
                GtkWidget *parent,
                GtkWidget *widget,
                GdkRGBA *color)
{
  gint x, y, w, h, xw, yh, y_top, yh_bottom, x_left, xw_right;
  gint top, bottom, left, right;
  cairo_pattern_t *gradient;
  gdouble r, g, b, cx, cy;
  GtkAllocation alloc;

  gtk_widget_get_allocation (widget, &alloc);

  if (alloc.x < 0 || alloc.y < 0) return;
  
  r = color->red; g = color->green; b = color->blue;
  gtk_widget_translate_coordinates (widget, parent, 0, 0, &x, &y);

  w = alloc.width;
  h = alloc.height;
  xw = x + w;
  yh = y + h;

  top = gtk_widget_get_margin_top (widget);
  bottom = gtk_widget_get_margin_bottom (widget);
  left = gtk_widget_get_margin_left (widget);
  right = gtk_widget_get_margin_right (widget);
  
  y_top = y - top;
  yh_bottom = yh + bottom;
  x_left = x - left;
  xw_right = xw + right;
  
  /* Draw widget area overlay */
  cx = x + w/2;
  cy = y + h/2;
  gradient = cairo_pattern_create_radial (cx, cy, MIN (w, h)/6, cx, cy, MAX (w, h)/2);
  cairo_pattern_add_color_stop_rgba (gradient, 0, r+.24, g+.24, b+.24, .16);
  cairo_pattern_add_color_stop_rgba (gradient, 1, r, g, b, .28);
  cairo_set_source (cr, gradient);

  cairo_rectangle (cr, x, y, w, h);
  cairo_fill (cr);

  cairo_pattern_destroy (gradient);

  /* Draw margins overlays */
  if (top)
    draw_margin_selection (cr, x, xw, xw_right, x_left, y, y, y_top, y_top,
                           r, g, b, x, y_top);

  if (bottom)
    draw_margin_selection (cr, x, xw, xw_right, x_left, yh, yh, yh_bottom, yh_bottom,
                           r, g, b, x, yh_bottom);

  if (left)
    draw_margin_selection (cr, x, x, x_left, x_left, y, yh, yh_bottom, y_top,
                           r, g, b, x_left, y);

  if (right)
    draw_margin_selection (cr, xw, xw, xw_right, xw_right, y, yh, yh_bottom, y_top,
                           r, g, b, xw_right, y);

  /* Draw Selection box */
  cairo_set_source_rgba (cr, r, g, b, .75);
  cairo_rectangle (cr, x - left, y - top, w + left + right, h + top + bottom);
  cairo_stroke (cr);
}

#define DIMENSION_OFFSET 9
#define DIMENSION_LINE_OFFSET 4

static void
draw_hmark (cairo_t *cr, gdouble x, gdouble y)
{
  cairo_move_to (cr, x + 2, y - 2);
  cairo_line_to (cr, x - 2, y + 2);
}

static void
draw_vmark (cairo_t *cr, gdouble x, gdouble y)
{
  cairo_move_to (cr, x - 2, y - 2);
  cairo_line_to (cr, x + 2, y + 2);
}

static void
draw_vguide (cairo_t *cr, gdouble x, gdouble y, gint len)
{
  cairo_move_to (cr, x, y - DIMENSION_LINE_OFFSET);
  cairo_line_to (cr, x, y + len);
}

static void
draw_hguide (cairo_t *cr, gdouble x, gdouble y, gint len)
{
  cairo_move_to (cr, x + DIMENSION_LINE_OFFSET, y);
  cairo_line_to (cr, x - len, y);
}

static void
draw_pixel_value (cairo_t *cr, 
                  GdkRGBA *bg, GdkRGBA *fg,
                  gdouble x, gdouble y,
                  gboolean rotate,
                  gboolean draw_border,
                  gint val)
{
  cairo_text_extents_t extents;
  gchar pixel_str[8];
  gdouble xx, yy;

  g_snprintf (pixel_str, 8, "%d", val);

  cairo_text_extents (cr, pixel_str, &extents);

  if (rotate)
    {
      xx = x - 1.5;
      yy = y + .5 + extents.width/2;
      cairo_rotate (cr, G_PI/-2);
      cairo_device_to_user (cr, &xx, &yy);
    }
  else
    {
      xx = x - (extents.width+extents.x_bearing)/2;
      yy = y - 2;
    }

  if (draw_border || extents.width + 4 >= val)
    {
      cairo_set_source_rgba (cr, bg->red, bg->green, bg->blue, .9);

      cairo_move_to (cr, xx, yy);
      cairo_text_path (cr, pixel_str);
      cairo_set_line_width (cr, 3);
      cairo_stroke (cr);

      cairo_set_line_width (cr, 1);
      gdk_cairo_set_source_rgba (cr, fg);
    }

  cairo_move_to (cr, xx, yy);
  cairo_show_text (cr, pixel_str);

  if (rotate) cairo_rotate (cr, G_PI/2);
}

static void
draw_stroke_lines (cairo_t *cr, GdkRGBA *bg, GdkRGBA *fg, gboolean remark)
{
  if (remark)
    {
      cairo_set_source_rgba (cr, bg->red, bg->green, bg->blue, .9);
      cairo_set_line_width (cr, 3);
      cairo_stroke_preserve (cr);
      cairo_set_line_width (cr, 1);
    }

  gdk_cairo_set_source_rgba (cr, fg);
  cairo_stroke (cr);
}

static void
draw_dimensions (cairo_t *cr,
                 GdkRGBA *bg, GdkRGBA *fg,
                 gdouble x, gdouble y,
                 gint w, gint h,
                 gint top, gint bottom,
                 gint left, gint right)
{
  gboolean h_clutter, v_clutter;
  gdouble xx, yy;
  GdkRGBA color;

  w--; h--;
  xx = x + w + DIMENSION_OFFSET;
  yy = y - DIMENSION_OFFSET;
  h_clutter = top < DIMENSION_OFFSET*2;
  v_clutter = right < (DIMENSION_OFFSET + OUTLINE_WIDTH);

  /* Color half way betwen fg and bg */
  color.red = ABS (bg->red - fg->red)/2;
  color.green = ABS (bg->green - fg->green)/2;
  color.blue = ABS (bg->blue - fg->blue)/2;
  color.alpha = fg->alpha;

  cairo_set_font_size (cr, 8.0);
  
  /* Draw dimension lines and guides */
  if (left || right)
    {
      /* Draw horizontal lines */
      cairo_move_to (cr, x - left - DIMENSION_LINE_OFFSET, yy);
      cairo_line_to (cr, x + w + right + DIMENSION_LINE_OFFSET, yy);

      if (top < DIMENSION_OFFSET)
        {
          draw_vguide (cr, x - left, yy, DIMENSION_OFFSET - top);
          draw_vguide (cr, x + w + right, yy, DIMENSION_OFFSET - top);
        }

      draw_vguide (cr, x, yy, DIMENSION_OFFSET);
      draw_vguide (cr, x + w, yy, DIMENSION_OFFSET);
      
      draw_stroke_lines (cr, bg, &color, top < DIMENSION_OFFSET+OUTLINE_WIDTH);

      /* Draw dimension line marks */
      if (left) draw_hmark (cr, x - left, yy);
      draw_hmark (cr, x, yy);
      draw_hmark (cr, x + w, yy);
      if (right) draw_hmark (cr, x + w + right, yy);

      draw_stroke_lines (cr, bg, fg, top < DIMENSION_OFFSET+OUTLINE_WIDTH);

      /* Draw pixel values */
      draw_pixel_value (cr, bg, fg, x + w/2, yy, FALSE, h_clutter, w+1);
      if (left) draw_pixel_value (cr,bg, fg, x - left/2, yy, FALSE, h_clutter, left);
      if (right) draw_pixel_value (cr,bg, fg, x + w + right/2, yy, FALSE, h_clutter, right);
    }
  
  if (top || bottom)
    {
      /* Draw vertical lines */
      cairo_move_to (cr, xx, y - top - DIMENSION_LINE_OFFSET);
      cairo_line_to (cr, xx, y + h + bottom + DIMENSION_LINE_OFFSET);

      if (right < DIMENSION_OFFSET)
        {
          draw_hguide (cr, xx, y - top, DIMENSION_OFFSET - right);
          draw_hguide (cr, xx, y + h + bottom, DIMENSION_OFFSET - right);
        }

      draw_hguide (cr, xx, y, DIMENSION_OFFSET);
      draw_hguide (cr, xx, y + h, DIMENSION_OFFSET);

      draw_stroke_lines (cr, bg, &color, v_clutter);

      /* Draw marks */
      if (top) draw_vmark (cr, xx, y - top);
      draw_vmark (cr, xx, y);
      draw_vmark (cr, xx, y + h);
      if (bottom) draw_vmark (cr, xx, y + h + bottom);

      draw_stroke_lines (cr, bg, fg, v_clutter);

      /* Draw pixel values */
      draw_pixel_value (cr,bg, fg, xx, y + h/2, TRUE, v_clutter, h+1);
      if (top) draw_pixel_value (cr,bg, fg, xx, y - top/2, TRUE, v_clutter, top);
      if (bottom) draw_pixel_value (cr,bg, fg, xx, y + h + bottom/2, TRUE, v_clutter, bottom);
    }
}

static void 
draw_pushpin (cairo_t *cr, gdouble x, gdouble y, gint angle,
              GdkRGBA *outline, GdkRGBA *fill, GdkRGBA *outline2, GdkRGBA *fg,
              gboolean over, gboolean active)
{
  cairo_save (cr);

  if (active)
    {
      outline = outline2;
      x += .5;
      cairo_rotate (cr, angle*(G_PI/180));
      cairo_device_to_user (cr, &x, &y);
    }
  else
    x += 1.5;

  /* Swap colors if mouse is over */
  if (over)
    {
      GdkRGBA *tmp = outline;
      outline = fill;
      fill = tmp;
    }
  
  cairo_translate (cr, x, y);

  _glade_design_layout_draw_pushpin (cr, (active) ? 2.5 : 4, outline, fill,
                                     (over) ? outline : fill, fg);
  
  cairo_restore (cr);
}

static inline void
draw_selection_nodes (cairo_t *cr,
                      GladeDesignLayoutPrivate *priv,
                      GtkWidget *parent)
{
  GladePointerMode mode = glade_project_get_pointer_mode (priv->project);
  Margins node = priv->node_over;
  GtkWidget *widget = priv->selection;
  gint top, bottom, left, right;
  gint x1, x2, x3, y1, y2, y3;
  GtkAllocation alloc, palloc;
  GdkRGBA *c1, *c2, *c3, *fg;
  gint x, y, w, h;

  gtk_widget_get_allocation (widget, &alloc);
  if (alloc.x < 0 || alloc.y < 0) return;

  c1 = &priv->frame_color_active[0];
  c2 = &priv->frame_color_active[1];
  c3 = &priv->frame_color[0];
  fg = &priv->fg_color;
  
  gtk_widget_get_allocation (parent, &palloc);
  
  w = alloc.width;
  h = alloc.height;
  
  gtk_widget_translate_coordinates (widget, parent, 0, 0, &x, &y);

  top = gtk_widget_get_margin_top (widget);
  bottom = gtk_widget_get_margin_bottom (widget);
  left = gtk_widget_get_margin_left (widget);
  right = gtk_widget_get_margin_right (widget);

  /* Draw nodes */
  x1 = x - left;
  x2 = x + w/2;
  x3 = x + w + right;
  y1 = y - top;
  y2 = y + h/2;
  y3 = y + h + bottom;

  /* Draw nodes */
  cairo_set_line_width (cr, OUTLINE_WIDTH);

  if (mode == GLADE_POINTER_MARGIN_EDIT)
    {
      _glade_design_layout_draw_node (cr, x2, y1, c1, c2);
      _glade_design_layout_draw_node (cr, x2, y3, c1, c2);
      _glade_design_layout_draw_node (cr, x1, y2, c1, c2);
      _glade_design_layout_draw_node (cr, x3, y2, c1, c2);

      /* Draw dimensions */
      if (top || bottom || left || right)
        {
          cairo_set_line_width (cr, 1);
          draw_dimensions (cr, c2, fg, x+.5, y+.5, w, h, top, bottom, left, right);
        }
    }
  else if (mode == GLADE_POINTER_ALIGN_EDIT)
    {
      GtkAlign valign, halign;
      
      valign = gtk_widget_get_valign (widget);
      halign = gtk_widget_get_halign (widget);
      
      if (valign == GTK_ALIGN_FILL)
        {
          draw_pushpin (cr, x2, y1, 45, c3, c2, c1, fg, node & MARGIN_TOP, TRUE);
          draw_pushpin (cr, x2, y3-4, -45, c3, c2, c1, fg, node & MARGIN_BOTTOM, TRUE);
        }
      else
        {
          draw_pushpin (cr, x2, y1, 45, c3, c2, c1, fg, node & MARGIN_TOP, valign == GTK_ALIGN_START);
          draw_pushpin (cr, x2, y3-4, -45, c3, c2, c1, fg, node & MARGIN_BOTTOM, valign == GTK_ALIGN_END);
        }

      if (halign == GTK_ALIGN_FILL)
        {
          draw_pushpin (cr, x1, y2, -45, c3, c2, c1, fg, node & MARGIN_LEFT, TRUE);
          draw_pushpin (cr, x3, y2, 45, c3, c2, c1, fg, node & MARGIN_RIGHT, TRUE);
        }
      else
        {
          draw_pushpin (cr, x1, y2, -45, c3, c2, c1, fg, node & MARGIN_LEFT, halign == GTK_ALIGN_START);
          draw_pushpin (cr, x3, y2, 45, c3, c2, c1, fg, node & MARGIN_RIGHT, halign == GTK_ALIGN_END);
        }
    }
}

static gboolean
glade_design_layout_draw (GtkWidget *widget, cairo_t *cr)
{
  GladeDesignLayoutPrivate *priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);
  GdkWindow *window = gtk_widget_get_window (widget);

  if (gtk_cairo_should_draw_window (cr, window))
    {
      GtkWidget *child;

      if ((child = gtk_bin_get_child (GTK_BIN (widget))) &&
          gtk_widget_get_visible (child))
        {
          gint border_width = gtk_container_get_border_width (GTK_CONTAINER (widget));
          gboolean selected = FALSE;
          GList *l;

          /* draw offscreen widgets */
          gdk_cairo_set_source_window (cr, priv->offscreen_window,
                                       priv->child_offset, priv->child_offset);
          cairo_rectangle (cr, priv->child_offset, priv->child_offset,
                           priv->current_width, priv->current_height);
          cairo_fill (cr);

          /* Draw selection */
          cairo_set_line_width (cr, OUTLINE_WIDTH/2);
          cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
          cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
          for (l = glade_project_selection_get (priv->project); l; l = g_list_next (l))
            {
              GtkWidget *selection = l->data;
              
              /* Dont draw selection on toplevels */
              if (child != selection)
                {
                  if (GTK_IS_WIDGET (selection) && 
                      gtk_widget_is_ancestor (selection, child))
                  {
                    draw_selection (cr, widget, selection, &priv->frame_color_active[0]);
                    selected = TRUE;
                  }
                }
              else
                selected = TRUE;
            }

          /* draw frame */
          draw_frame (cr, priv, selected,
                      border_width + PADDING,
                      border_width + PADDING,
                      priv->current_width + 2 * OUTLINE_WIDTH,
                      priv->current_height + 2 * OUTLINE_WIDTH);

          /* Draw selection nodes if we are in margins edit mode */
          if (priv->selection)
            draw_selection_nodes (cr, priv, widget);
        }
    }
  else if (gtk_cairo_should_draw_window (cr, priv->offscreen_window))
    {
      GtkWidget *child = gtk_bin_get_child (GTK_BIN (widget));

      gtk_render_background (priv->default_context, cr, 0, 0,
                             gdk_window_get_width (priv->offscreen_window),
                             gdk_window_get_height (priv->offscreen_window));

      if (child)
        gtk_container_propagate_draw (GTK_CONTAINER (widget), child, cr);
    }

  return FALSE;
}

static inline void
to_child (GladeDesignLayout *bin,
          double         widget_x,
          double         widget_y,
          double        *x_out,
          double        *y_out)
{
  GladeDesignLayoutPrivate *priv = bin->priv;
  *x_out = widget_x - priv->child_offset;
  *y_out = widget_y - priv->child_offset;
}

static inline void
to_parent (GladeDesignLayout *bin,
           double         offscreen_x,
           double         offscreen_y,
           double        *x_out,
           double        *y_out)
{
  GladeDesignLayoutPrivate *priv = bin->priv;
  *x_out = offscreen_x + priv->child_offset;
  *y_out = offscreen_y + priv->child_offset;
}

static GdkWindow *
pick_offscreen_child (GdkWindow     *offscreen_window,
                      double         widget_x,
                      double         widget_y,
                      GladeDesignLayout *bin)
{
  GladeDesignLayoutPrivate *priv = bin->priv;
  GtkWidget *child = gtk_bin_get_child (GTK_BIN (bin));

  if (child && gtk_widget_get_visible (child))
    {
      GtkAllocation child_area;
      double x, y;

      to_child (bin, widget_x, widget_y, &x, &y);
        
      gtk_widget_get_allocation (child, &child_area);

      if (x >= 0 && x < child_area.width && y >= 0 && y < child_area.height)
        return (priv->selection) ? NULL : priv->offscreen_window;
    }

  return NULL;
}

static void
offscreen_window_to_parent (GdkWindow     *offscreen_window,
                            double         offscreen_x,
                            double         offscreen_y,
                            double        *parent_x,
                            double        *parent_y,
                            GladeDesignLayout *bin)
{
  to_parent (bin, offscreen_x, offscreen_y, parent_x, parent_y);
}

static void
offscreen_window_from_parent (GdkWindow     *window,
                              double         parent_x,
                              double         parent_y,
                              double        *offscreen_x,
                              double        *offscreen_y,
                              GladeDesignLayout *bin)
{
  to_child (bin, parent_x, parent_y, offscreen_x, offscreen_y);
}


static void
glade_design_layout_realize (GtkWidget * widget)
{
  GladeDesignLayoutPrivate *priv;
  GdkWindowAttr attributes;
  gint attributes_mask, border_width;
  GtkAllocation allocation;
  GdkDisplay *display;
    
  priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);

  gtk_widget_set_realized (widget, TRUE);
    
  gtk_widget_get_allocation (widget, &allocation);
  border_width = gtk_container_get_border_width (GTK_CONTAINER (widget));

  attributes.x = allocation.x + border_width;
  attributes.y = allocation.y + border_width;
  attributes.width = allocation.width - 2 * border_width;
  attributes.height = allocation.height - 2 * border_width;
  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.event_mask = gtk_widget_get_events (widget)
                        | GDK_EXPOSURE_MASK
                        | GDK_POINTER_MOTION_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_SCROLL_MASK
                        | GDK_ENTER_NOTIFY_MASK
                        | GDK_LEAVE_NOTIFY_MASK;

  attributes.visual = gtk_widget_get_visual (widget);
  attributes.wclass = GDK_INPUT_OUTPUT;

  attributes_mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL;

  priv->window = gdk_window_new (gtk_widget_get_parent_window (widget),
                                 &attributes, attributes_mask);
  gtk_widget_set_window (widget, priv->window);
  gdk_window_set_user_data (priv->window, widget);

  g_signal_connect (priv->window, "pick-embedded-child",
                    G_CALLBACK (pick_offscreen_child), widget);

  /* Offscreen window */
  attributes.window_type = GDK_WINDOW_OFFSCREEN;
  attributes.x = attributes.y = 0;
  attributes.width = attributes.height = 0;

  priv->offscreen_window = gdk_window_new (gtk_widget_get_root_window (widget),
                                           &attributes, attributes_mask);
  gdk_window_set_user_data (priv->offscreen_window, widget);
  
  gdk_offscreen_window_set_embedder (priv->offscreen_window, priv->window);

  g_signal_connect (priv->offscreen_window, "to-embedder",
                    G_CALLBACK (offscreen_window_to_parent), widget);
  g_signal_connect (priv->offscreen_window, "from-embedder",
                    G_CALLBACK (offscreen_window_from_parent), widget);

  gdk_window_show (priv->offscreen_window);

  gdk_window_set_cursor (priv->window, NULL);
  gdk_window_set_cursor (priv->offscreen_window, NULL);
  
  /* Allocate cursors */
  display = gtk_widget_get_display (widget);
  priv->cursors[ACTIVITY_RESIZE_HEIGHT] = gdk_cursor_new_for_display (display, GDK_BOTTOM_SIDE);
  priv->cursors[ACTIVITY_RESIZE_WIDTH] = gdk_cursor_new_for_display (display, GDK_RIGHT_SIDE);
  priv->cursors[ACTIVITY_RESIZE_WIDTH_AND_HEIGHT] = gdk_cursor_new_for_display (display, GDK_BOTTOM_RIGHT_CORNER);
  
  priv->cursors[ACTIVITY_MARGINS_VERTICAL] = gdk_cursor_new_for_display (display, GDK_SB_V_DOUBLE_ARROW);
  priv->cursors[ACTIVITY_MARGINS_HORIZONTAL] = gdk_cursor_new_for_display (display, GDK_SB_H_DOUBLE_ARROW);
  priv->cursors[ACTIVITY_MARGINS_TOP_LEFT] = gdk_cursor_new_for_display (display, GDK_TOP_LEFT_CORNER);
  priv->cursors[ACTIVITY_MARGINS_TOP_RIGHT] = gdk_cursor_new_for_display (display, GDK_TOP_RIGHT_CORNER);
  priv->cursors[ACTIVITY_MARGINS_BOTTOM_LEFT] = gdk_cursor_new_for_display (display, GDK_BOTTOM_LEFT_CORNER);
  priv->cursors[ACTIVITY_MARGINS_BOTTOM_RIGHT] = g_object_ref (priv->cursors[ACTIVITY_RESIZE_WIDTH_AND_HEIGHT]);
  
  priv->widget_name = pango_layout_new (gtk_widget_get_pango_context (widget));
}

static void
glade_design_layout_unrealize (GtkWidget * widget)
{
  GladeDesignLayoutPrivate *priv;
  gint i;
  
  priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);

  if (priv->offscreen_window)
    {
      gdk_window_set_user_data (priv->offscreen_window, NULL);
      gdk_window_destroy (priv->offscreen_window);
      priv->offscreen_window = NULL;
    }

  /* Free cursors */
  for (i = 0; i < N_ACTIVITY; i++)
    {
      if (priv->cursors[i])
        {
          g_object_unref (priv->cursors[i]);
          priv->cursors[i] = NULL;
        }
    }

  priv->cursor = NULL;

  if (priv->widget_name)
    {
      g_object_unref (priv->widget_name);
      priv->widget_name = NULL;
    }
  
  GTK_WIDGET_CLASS (glade_design_layout_parent_class)->unrealize (widget);
}

static void
glade_design_layout_style_updated (GtkWidget *widget)
{
  GladeDesignLayoutPrivate *priv = GLADE_DESIGN_LAYOUT_GET_PRIVATE (widget);
  
  _glade_design_layout_get_colors (gtk_widget_get_style_context (widget),
                                   &priv->frame_color[0],
                                   &priv->frame_color[1],
                                   &priv->frame_color_active[0],
                                   &priv->frame_color_active[1]);

  priv->fg_color = priv->frame_color[1];
}

static void
glade_design_layout_init (GladeDesignLayout *layout)
{
  GtkWidgetPath *path = gtk_widget_path_new ();
  GladeDesignLayoutPrivate *priv;
  gint i;
  
  layout->priv = priv = GLADE_DESIGN_LAYOUT_GET_PRIVATE (layout);

  priv->activity = ACTIVITY_NONE;

  for (i = 0; i < N_ACTIVITY; i++) priv->cursors[i] = NULL;

  priv->new_width = -1;
  priv->new_height = -1;
  priv->node_over = 0;

  priv->default_context = gtk_style_context_new ();
  gtk_widget_path_append_type (path, GTK_TYPE_WINDOW);
  gtk_style_context_set_path (priv->default_context, path);

  /* setup static member of rectangles */
  priv->east.width = PADDING + OUTLINE_WIDTH;
  priv->south.height = PADDING + OUTLINE_WIDTH;

  gtk_widget_set_has_window (GTK_WIDGET (layout), TRUE);

  gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (layout)),
                               GTK_STYLE_CLASS_VIEW);

  gtk_widget_path_unref (path);
}

static void
on_pointer_mode_notify (GladeProject *project,
                        GParamSpec *pspec, 
                        GladeDesignLayout *layout)
{
  GladeDesignLayoutPrivate *priv = layout->priv;
  GladePointerMode mode;
  GtkWidget *selection;
 
  g_return_if_fail (priv->window);

  mode = glade_project_get_pointer_mode (priv->project);
  if (mode == GLADE_POINTER_MARGIN_EDIT || mode == GLADE_POINTER_ALIGN_EDIT)
    {
      GList *l = glade_project_selection_get (project);
      selection = (l && g_list_next (l) == NULL && GTK_IS_WIDGET (l->data)) ? l->data : NULL;
      gdl_edit_mode_set_selection (layout, mode, NULL);
    }
  else
    selection = NULL;

  gdl_edit_mode_set_selection (layout, mode, selection);
  gdk_window_invalidate_rect (priv->window, NULL, FALSE);
}

static void
glade_design_layout_set_property (GObject *object,
                                  guint prop_id,
                                  const GValue *value,
                                  GParamSpec *pspec)
{
  switch (prop_id)
    {
      case PROP_DESIGN_VIEW:
        {
          GladeDesignLayoutPrivate *priv = GLADE_DESIGN_LAYOUT_PRIVATE (object);
          priv->view = GLADE_DESIGN_VIEW (g_value_get_object (value));
          priv->project = glade_design_view_get_project (priv->view);
          g_signal_connect (priv->project, "notify::pointer-mode",
                            G_CALLBACK (on_pointer_mode_notify),
                            GLADE_DESIGN_LAYOUT (object));
        }
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
glade_design_layout_get_property (GObject *object,
                                  guint prop_id,
                                  GValue *value,
                                  GParamSpec *pspec)
{
  switch (prop_id)
    {
      case PROP_DESIGN_VIEW:
        g_value_set_object (value, GLADE_DESIGN_LAYOUT_PRIVATE (object)->view);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
on_project_selection_changed (GladeProject *project, GladeDesignLayout *layout)
{
  GladeDesignLayoutPrivate *priv = layout->priv;
  GladePointerMode mode = glade_project_get_pointer_mode (project);

  if (priv->selection)
    gdl_edit_mode_set_selection (layout, GLADE_POINTER_SELECT, NULL);
  else if (mode == GLADE_POINTER_ALIGN_EDIT || mode == GLADE_POINTER_MARGIN_EDIT)
    {
      GList *l = glade_project_selection_get (project);
      gdl_edit_mode_set_selection (layout, mode, (l) ? l->data : NULL);
    }
}

static GObject *
glade_design_layout_constructor (GType                  type,
                                 guint                  n_construct_params,
                                 GObjectConstructParam *construct_params)
{
  GladeDesignLayoutPrivate *priv;
  GObject *object;
    
  object = G_OBJECT_CLASS (glade_design_layout_parent_class)->constructor (type,
                                                                           n_construct_params,
                                                                           construct_params);

  priv = GLADE_DESIGN_LAYOUT_PRIVATE (object);

  g_signal_connect (priv->project,
                    "selection-changed",
                    G_CALLBACK (on_project_selection_changed),
                    GLADE_DESIGN_LAYOUT (object));

  glade_design_layout_style_updated (GTK_WIDGET (object));

  return object;
}

static void
glade_design_layout_finalize (GObject *object)
{
  GladeDesignLayout *layout = GLADE_DESIGN_LAYOUT (object);
  GladeDesignLayoutPrivate *priv = layout->priv;

  g_clear_object (&priv->default_context);

  g_signal_handlers_disconnect_by_func (priv->project,
                                        on_project_selection_changed,
                                        layout);
  g_signal_handlers_disconnect_by_func (priv->project,
                                        on_pointer_mode_notify,
                                        layout);

  G_OBJECT_CLASS (glade_design_layout_parent_class)->finalize (object);
}

static void
glade_design_layout_drag_begin (GtkWidget *widget, GdkDragContext *context)
{
  GladeDesignLayoutPrivate *priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);
  cairo_pattern_t *pattern;
  cairo_surface_t *surface;
  GtkAllocation alloc;
  GtkWidget *window;
  GdkScreen *screen;
  cairo_t *cr;
  gint x, y;

  gtk_widget_get_allocation (priv->drag_source, &alloc);

  gtk_widget_translate_coordinates (priv->drag_source, widget,
                                    alloc.x, alloc.y,
                                    &x, &y);

  screen = gdk_window_get_screen (gdk_drag_context_get_source_window (context));
  window = gtk_window_new (GTK_WINDOW_POPUP);
  gtk_window_set_type_hint (GTK_WINDOW (window), GDK_WINDOW_TYPE_HINT_DND);
  gtk_window_set_screen (GTK_WINDOW (window), screen);

  gtk_widget_set_events (window, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
  gtk_widget_set_app_paintable (window, TRUE);

  gtk_widget_set_size_request (window, alloc.width, alloc.height);
  gtk_widget_realize (window);

  surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24, alloc.width, alloc.height);
  cr = cairo_create (surface);

  gdk_cairo_set_source_window (cr, priv->window, alloc.x - x, alloc.y - y);
  cairo_paint (cr);
  cairo_surface_flush (surface);
  
  pattern = cairo_pattern_create_for_surface (surface);
  
  gdk_window_set_background_pattern (gtk_widget_get_window (window), pattern);

  gtk_window_set_opacity (GTK_WINDOW (window), .5);
  gtk_drag_set_icon_widget (context, window, priv->drag_x, priv->drag_y);

  cairo_destroy (cr);
  cairo_pattern_destroy (pattern);
  cairo_surface_destroy (surface);

  priv->drag_icon = g_object_ref_sink (window);
}

static void
glade_design_layout_drag_data_get (GtkWidget        *widget,
                                   GdkDragContext   *context,
                                   GtkSelectionData *data,
                                   guint             info,
                                   guint             time)
{
  GladeDesignLayoutPrivate *priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);

  if (priv->drag_source)
    {
      static GdkAtom type = 0;

      if (!type)
        type = gdk_atom_intern_static_string (GDL_DND_TARGET_WIDGET);

      gtk_selection_data_set (data, type, sizeof (gpointer),
                              (const guchar *)&priv->drag_source, sizeof (gpointer));
    }
}

static void
glade_design_layout_drag_end (GtkWidget *widget, GdkDragContext *context)
{
  GladeDesignLayoutPrivate *priv = GLADE_DESIGN_LAYOUT_PRIVATE (widget);

  g_clear_object (&priv->drag_icon);
  priv->drag_source = NULL;
}

static void
glade_design_layout_class_init (GladeDesignLayoutClass * klass)
{
  GObjectClass *object_class;
  GtkWidgetClass *widget_class;
  GtkContainerClass *container_class;

  object_class = G_OBJECT_CLASS (klass);
  widget_class = GTK_WIDGET_CLASS (klass);
  container_class = GTK_CONTAINER_CLASS (klass);

  object_class->constructor = glade_design_layout_constructor;
  object_class->finalize = glade_design_layout_finalize;
  object_class->set_property = glade_design_layout_set_property;
  object_class->get_property = glade_design_layout_get_property;
  
  container_class->add = glade_design_layout_add;
  container_class->remove = glade_design_layout_remove;

  widget_class->realize = glade_design_layout_realize;
  widget_class->unrealize = glade_design_layout_unrealize;
  widget_class->motion_notify_event = glade_design_layout_motion_notify_event;
  widget_class->leave_notify_event = glade_design_layout_leave_notify_event;
  widget_class->button_press_event = glade_design_layout_button_press_event;
  widget_class->button_release_event = glade_design_layout_button_release_event;
  widget_class->draw = glade_design_layout_draw;
  widget_class->get_preferred_height = glade_design_layout_get_preferred_height;
  widget_class->get_preferred_width = glade_design_layout_get_preferred_width;
  widget_class->get_preferred_width_for_height = glade_design_layout_get_preferred_width_for_height;
  widget_class->get_preferred_height_for_width = glade_design_layout_get_preferred_height_for_width;
  widget_class->size_allocate = glade_design_layout_size_allocate;
  widget_class->style_updated = glade_design_layout_style_updated;
  widget_class->drag_begin = glade_design_layout_drag_begin;
  widget_class->drag_end = glade_design_layout_drag_end;
  widget_class->drag_data_get = glade_design_layout_drag_data_get;

  g_object_class_install_property (object_class, PROP_DESIGN_VIEW,
                                   g_param_spec_object ("design-view", _("Design View"),
                                                        _("The GladeDesignView that contains this layout"),
                                                        GLADE_TYPE_DESIGN_VIEW,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  
  g_signal_override_class_closure (g_signal_lookup ("damage-event", GTK_TYPE_WIDGET),
                                   GLADE_TYPE_DESIGN_LAYOUT,
                                   g_cclosure_new (G_CALLBACK (glade_design_layout_damage),
                                                   NULL, NULL));

  g_type_class_add_private (object_class, sizeof (GladeDesignLayoutPrivate));
}

/* Internal API */

GtkWidget *
_glade_design_layout_new (GladeDesignView *view)
{
  return g_object_new (GLADE_TYPE_DESIGN_LAYOUT, "design-view", view, NULL);
}

void 
_glade_design_layout_draw_node (cairo_t *cr,
                                gdouble x,
                                gdouble y,
                                GdkRGBA *fg,
                                GdkRGBA *bg)
{
  cairo_new_sub_path (cr);
  cairo_arc (cr, x, y, OUTLINE_WIDTH, 0, 2*G_PI);

  gdk_cairo_set_source_rgba (cr, bg);
  cairo_stroke_preserve (cr);

  gdk_cairo_set_source_rgba (cr, fg);
  cairo_fill (cr);
}

void 
_glade_design_layout_draw_pushpin (cairo_t *cr,
                                   gdouble needle_length,
                                   GdkRGBA *outline,
                                   GdkRGBA *fill,
                                   GdkRGBA *bg,
                                   GdkRGBA *fg)
{
  cairo_save (cr);

  /* Draw needle */
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
  cairo_set_line_width (cr, 1);
  
  cairo_move_to (cr, 1, 2);
  cairo_line_to (cr, 1, 2+needle_length);
  cairo_set_source_rgba (cr, bg->red, bg->green, bg->blue, .9);
  cairo_stroke(cr);
  
  cairo_move_to (cr, 0, 2);
  cairo_line_to (cr, 0, 2+needle_length);
  gdk_cairo_set_source_rgba (cr, fg);
  cairo_stroke (cr);

  /* Draw top and bottom fat lines */
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

  cairo_move_to (cr, -4, 0);
  cairo_line_to (cr, 4, 0);
  
  cairo_move_to (cr, -2.5, -7);
  cairo_line_to (cr, 2.5, -7);

  gdk_cairo_set_source_rgba (cr, outline);
  cairo_set_line_width (cr, 4);
  cairo_stroke_preserve (cr);

  gdk_cairo_set_source_rgba (cr, fill);
  cairo_set_line_width (cr, 2);
  cairo_stroke (cr);

  /* Draw middle section */
  cairo_move_to (cr, -2, -5);
  cairo_line_to (cr, 2, -5);
  cairo_line_to (cr, 3, -2);
  cairo_line_to (cr, -3, -2);
  cairo_close_path (cr);

  gdk_cairo_set_source_rgba (cr, outline);
  cairo_set_line_width (cr, 2);
  cairo_stroke_preserve (cr);
  gdk_cairo_set_source_rgba (cr, fill);
  cairo_fill (cr);

  /* Draw middle section shadow */
  cairo_set_source_rgb (cr, fill->red-.16, fill->green-.16, fill->blue-.16);
  cairo_set_line_width (cr, 1);
  cairo_move_to (cr, 1, -5);
  cairo_line_to (cr, 1.5, -2);
  cairo_stroke (cr);
  
  cairo_restore (cr);
}

static inline void
_glade_design_layout_coords_from_event (GdkWindow *parent,
                                        GdkEvent *event,
                                        gint *x, gint *y)
{
  GdkWindow *child = event->any.window;
  gdouble xx, yy;

  if (!gdk_event_get_coords (event, &xx, &yy))
    {
      *x = *y = 0;
      g_warning ("wrong event type %d", event->type);
      return;
    }
  
  while (child && parent != child)
    {
      gdk_window_coords_to_parent (child, xx, yy, &xx, &yy);
      child = gdk_window_get_parent (child);
    }

  *x = xx;
  *y = yy;
}

void
_glade_design_layout_get_colors (GtkStyleContext *context, 
                                 GdkRGBA *c1, GdkRGBA *c2,
                                 GdkRGBA *c3, GdkRGBA *c4)
{
  gfloat off;
  
  gtk_style_context_get_background_color (context, GTK_STATE_FLAG_NORMAL, c1);
  gtk_style_context_get_color (context, GTK_STATE_FLAG_NORMAL, c2);

  gtk_style_context_get_background_color (context, GTK_STATE_FLAG_SELECTED | GTK_STATE_FLAG_FOCUSED, c3);
  gtk_style_context_get_color (context, GTK_STATE_FLAG_SELECTED | GTK_STATE_FLAG_FOCUSED, c4);

  off = ((c1->red + c1->green + c1->blue)/3 < .5) ? .16 : -.16;
   
  c1->red += off;
  c1->green += off;
  c1->blue += off;
}

GtkTargetEntry *
_glade_design_layout_get_dnd_target (void)
{
  static GtkTargetEntry target = {GDL_DND_TARGET_WIDGET, GTK_TARGET_SAME_APP, GDL_DND_INFO_WIDGET};
  return &target;
}

void
_glade_design_layout_get_hot_point (GladeDesignLayout *layout,
                                    gint *x,
                                    gint *y)
{
  GladeDesignLayoutPrivate *priv = layout->priv;

  if (x)
    *x = priv->drag_x;

  if (y)
    *y = priv->drag_y;
}

static gboolean
widget_is_inside_fixed (GladeWidget *widget)
{
  while (widget)
    {
      if (GTK_IS_FIXED (glade_widget_get_object (widget)))
        return TRUE;
      widget = glade_widget_get_parent (widget);
    }

  return FALSE;
}

/*
 * _glade_design_layout_do_event:
 * @layout: A #GladeDesignLayout
 * @event: an event to process
 *
 * Process events to make widget selection work. This function should be called
 * before the child widget get the event. See gdk_event_handler_set()
 *
 * Returns: true if the event was handled.
 */
gboolean
_glade_design_layout_do_event (GladeDesignLayout *layout, GdkEvent *event)
{
  GtkWidget *widget = GTK_WIDGET (layout);
  GladeFindInContainerData data = { widget, 0, };
  GladeDesignLayoutPrivate *priv;
  GladePointerMode mode;
  gboolean retval;
  GList *l;

  priv = layout->priv;

  _glade_design_layout_coords_from_event (priv->window, event, &data.x, &data.y);

  mode = glade_project_get_pointer_mode (priv->project);
  glade_design_layout_find_inside_container (widget, &data);

  if (event->type == GDK_BUTTON_PRESS && event->button.button == 1 &&
      ((event->button.state & GDK_SHIFT_MASK && mode == GLADE_POINTER_SELECT) ||
       mode == GLADE_POINTER_DRAG_RESIZE))
    {
      GObject *source;
              
      if (data.gwidget && (source = glade_widget_get_object (data.gwidget)) &&
          (event->button.state & GDK_SHIFT_MASK || !widget_is_inside_fixed (data.gwidget)))
        {
          priv->drag_source = GTK_WIDGET (source);

          gtk_widget_translate_coordinates (widget, priv->drag_source,
                                            data.x, data.y,
                                            &priv->drag_x, &priv->drag_y);
          return TRUE;
        }
    }
  
  /* Check if we want to enter in margin edit mode */
  if (event->type == GDK_BUTTON_PRESS && event->button.button == 1 &&
      mode != GLADE_POINTER_DRAG_RESIZE &&
      (l = glade_project_selection_get (priv->project)) &&
      g_list_next (l) == NULL && GTK_IS_WIDGET (l->data) && 
      gtk_widget_is_ancestor (l->data, widget))
    {
      if (gdl_get_margins_from_pointer (widget, l->data, data.x, data.y))
        {
          if (priv->selection == NULL)
            {
              gdl_edit_mode_set_selection (layout,
                                           (event->button.state & GDK_SHIFT_MASK) ? 
                                           GLADE_POINTER_ALIGN_EDIT : GLADE_POINTER_MARGIN_EDIT,
                                           l->data);
              return TRUE;
            }
          return FALSE;
        }
    }

  _glade_design_view_freeze (priv->view);
  
  /* Try the placeholder first */
  if (data.placeholder && gtk_widget_event (data.placeholder, event)) 
    retval = TRUE;
  else if (data.gwidget) /* Then we try a GladeWidget */
    retval = glade_widget_event (data.gwidget, event);
  else
    retval = FALSE;

  _glade_design_view_thaw (priv->view);

  return retval;
}
