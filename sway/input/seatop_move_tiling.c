#include <limits.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/util/edges.h>
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "sway/output.h"
#include "sway/tree/arrange.h"
#include "sway/tree/node.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "sway/desktop/animation.h"

// Thickness of the dropzone when dragging to the edge of a layout container,
// as a fraction of the smaller side of the hovered container
#define DROP_LAYOUT_BORDER_RATIO 0.2f

struct seatop_move_tiling_event {
	struct sway_container *con;
	struct sway_node *target_node;
	enum wlr_edges target_edge;
	double ref_lx, ref_ly; // cursor's x/y at start of op
	bool threshold_reached;
	bool insert_after_target;
	struct wlr_scene_rect *indicator_rect;
	struct wl_listener target_node_destroy; // Clears target_node if destroyed mid-drag
};

static void handle_end(struct sway_seat *seat) {
	struct seatop_move_tiling_event *e = seat->seatop_data;
	wl_list_remove(&e->target_node_destroy.link);
	wlr_scene_node_destroy(&e->indicator_rect->node);
	e->indicator_rect = NULL;
}

// Update target_node and its destruction listener
static void set_target_node(struct seatop_move_tiling_event *e, struct sway_node *node) {
	if (e->target_node == node) {
		return;
	}
	wl_list_remove(&e->target_node_destroy.link);
	e->target_node = node;
	if (node) {
		wl_signal_add(&node->events.destroy, &e->target_node_destroy);
	} else {
		wl_list_init(&e->target_node_destroy.link);
	}
}

static void handle_target_node_destroy(struct wl_listener *listener, void *data) {
	struct seatop_move_tiling_event *e =
		wl_container_of(listener, e, target_node_destroy);
	set_target_node(e, NULL);
}

static void handle_motion_prethreshold(struct sway_seat *seat) {
	struct seatop_move_tiling_event *e = seat->seatop_data;
	double cx = seat->cursor->cursor->x;
	double cy = seat->cursor->cursor->y;
	double sx = e->ref_lx;
	double sy = e->ref_ly;

	// Get the scaled threshold for the output. Even if the operation goes
	// across multiple outputs of varying scales, just use the scale for the
	// output that the cursor is currently on for simplicity.
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
			root->output_layout, cx, cy);
	double output_scale = wlr_output ? wlr_output->scale : 1;
	double threshold = config->tiling_drag_threshold * output_scale;
	threshold *= threshold;

	// If the threshold has been exceeded, start the actual drag
	if ((cx - sx) * (cx - sx) + (cy - sy) * (cy - sy) > threshold) {
		wlr_scene_node_set_enabled(&e->indicator_rect->node, true);
		e->threshold_reached = true;
		cursor_set_image(seat->cursor, "grab", NULL);
	}
}

static void update_indicator(struct seatop_move_tiling_event *e, struct wlr_box *box) {
	wlr_scene_node_set_position(&e->indicator_rect->node, box->x, box->y);
	wlr_scene_rect_set_size(e->indicator_rect, box->width, box->height);
}

// Bounding box (in layout coordinates) of the children of `group`, with their
// sizes scaled by `scale`. A scaled or scrolled workspace arranges the children
// independently of the group's own stored box, so this is what is on screen.
static struct wlr_box group_children_box(struct sway_container *group, double scale) {
	struct sway_container *child = group->pending.children->items[0];
	struct wlr_box box;
	node_get_box(&child->node, &box);
	int left = box.x;
	int top = box.y;
	int right = box.x + (int)round(box.width * scale);
	int bottom = box.y + (int)round(box.height * scale);
	for (int i = 1; i < group->pending.children->length; ++i) {
		child = group->pending.children->items[i];
		node_get_box(&child->node, &box);
		left = MIN(left, box.x);
		top = MIN(top, box.y);
		right = MAX(right, box.x + (int)round(box.width * scale));
		bottom = MAX(bottom, box.y + (int)round(box.height * scale));
	}
	return (struct wlr_box){
		.x = left,
		.y = top,
		.width = right - left,
		.height = bottom - top,
	};
}

static void handle_motion_postthreshold(struct sway_seat *seat) {
	struct seatop_move_tiling_event *e = seat->seatop_data;
	struct wlr_surface *surface = NULL;
	double sx, sy;
	struct sway_cursor *cursor = seat->cursor;
	struct sway_node *node = node_at_coords(seat,
			cursor->cursor->x, cursor->cursor->y, &surface, &sx, &sy);

	if (!node) {
		// Eg. hovered over a layer surface such as swaybar
		set_target_node(e, NULL);
		e->target_edge = WLR_EDGE_NONE;
		return;
	}
	if (node->type == N_WORKSPACE) {
		// Empty workspace
		set_target_node(e, node);
		e->target_edge = WLR_EDGE_NONE;

		struct wlr_box drop_box;
		if (layout_overview_workspaces_enabled()) {
			struct sway_output *output = node->sway_workspace->output;
			drop_box.x = output->lx + node->sway_workspace->jump.x;
			drop_box.y = output->ly + node->sway_workspace->jump.y;
			drop_box.width = node->sway_workspace->jump.width;
			drop_box.height = node->sway_workspace->jump.height;
		} else {
			workspace_get_box(node->sway_workspace, &drop_box);
		}
		update_indicator(e, &drop_box);
		return;
	}
	struct sway_container *con = node->sway_container;
	// If tiling container, exit
	if (container_is_floating(con)) {
		set_target_node(e, NULL);
		e->target_edge = WLR_EDGE_NONE;
		return;
	}
	// Deny moving within own workspace if this is the only child
	if (workspace_num_tiling_views(e->con->pending.workspace) == 1 &&
			con->pending.workspace == e->con->pending.workspace) {
		set_target_node(e, NULL);
		e->target_edge = WLR_EDGE_NONE;
		return;
	}
	struct sway_workspace *old_workspace = e->con->pending.workspace;
	bool move_parent = layout_get_type(old_workspace) == layout_modifiers_get_mode(old_workspace);
	struct sway_workspace *workspace = con->pending.workspace;
	enum sway_container_layout layout = layout_get_type(workspace);
	// Box of the hovered container in layout (screen) coordinates. Tiled
	// containers store their position unscaled, only the size is scaled by
	// the workspace scale.
	struct wlr_box leaf_box;
	node_get_box(&con->node, &leaf_box);
	double scale = layout_scale_enabled(workspace) ? layout_scale_get(workspace) : 1.0;
	int drop_layout_border = (int)round(
			fmin(leaf_box.width, leaf_box.height) * DROP_LAYOUT_BORDER_RATIO * scale);
	if (drop_layout_border < 1) {
		drop_layout_border = 1;
	}
	// Box of the whole group the drop applies to: the parent column/row when
	// the hovered container is a leaf of the workspace layout, otherwise the
	// hovered container itself.
	struct wlr_box group_box = leaf_box;
	if (move_parent && con->view) {
		struct sway_container *parent = con->pending.parent;
		if (parent && parent->pending.children &&
				parent->pending.children->length > 0) {
			con = parent;
			group_box = group_children_box(parent, scale);
		}
	}
	leaf_box.width = (int)round(leaf_box.width * scale);
	leaf_box.height = (int)round(leaf_box.height * scale);
	int thresh_top = leaf_box.y + drop_layout_border;
	int thresh_bottom = leaf_box.y + leaf_box.height - drop_layout_border;
	int thresh_left = leaf_box.x + drop_layout_border;
	int thresh_right = leaf_box.x + leaf_box.width - drop_layout_border;
	// Map transformed cursor to real workspace
	double cx = cursor->cursor->x;
	double cy = cursor->cursor->y;
	if (layout_overview_workspaces_enabled()) {
		layout_overview_workspaces_local_to_global(workspace, &cx, &cy);
	}
	enum wlr_edges edge = WLR_EDGE_NONE;
	if (cy < thresh_top) {
		edge = WLR_EDGE_TOP;
	} else if (cy > thresh_bottom) {
		edge = WLR_EDGE_BOTTOM;
	} else if (cx < thresh_left) {
		edge = WLR_EDGE_LEFT;
	} else if (cx > thresh_right) {
		edge = WLR_EDGE_RIGHT;
	}
	// An edge parallel to the layout drops the container next to the hovered
	// window (layout_drag_container_to_container() inserts it relative to that
	// window), so its indicator hugs the hovered window. The remaining edges
	// move, reorder or swap the whole target column/row, so their indicator
	// hugs the group box, and WLR_EDGE_NONE (swap) highlights all of it.
	bool vertical_edge = edge == WLR_EDGE_TOP || edge == WLR_EDGE_BOTTOM;
	bool parallel_edge = edge != WLR_EDGE_NONE &&
		(layout == L_HORIZ ? vertical_edge : !vertical_edge);
	struct wlr_box box = parallel_edge ? leaf_box : group_box;
	switch (edge) {
	case WLR_EDGE_TOP:
		box.height = drop_layout_border;
		break;
	case WLR_EDGE_BOTTOM:
		box.y = box.y + box.height - drop_layout_border;
		box.height = drop_layout_border;
		break;
	case WLR_EDGE_LEFT:
		box.width = drop_layout_border;
		break;
	case WLR_EDGE_RIGHT:
		box.x = box.x + box.width - drop_layout_border;
		box.width = drop_layout_border;
		break;
	case WLR_EDGE_NONE:
	default:
		// Swap: highlight the whole group
		break;
	}
	set_target_node(e, &con->node);
	e->target_edge = edge;
	if (layout_overview_workspaces_enabled()) {
		double x = box.x;
		double y = box.y;
		layout_overview_workspaces_global_to_local(workspace, &x, &y);
		box.x = round(x);
		box.y = round(y);
		box.height *= workspace->jump.scale;
		box.width *= workspace->jump.scale;
	}
	update_indicator(e, &box);
	// Set focus on container so we can scroll the view
	seat_set_focus(seat, node);
	arrange_workspace(workspace);
	return;
}

static void handle_pointer_motion(struct sway_seat *seat, uint32_t time_msec) {
	struct seatop_move_tiling_event *e = seat->seatop_data;
	if (e->threshold_reached) {
		handle_motion_postthreshold(seat);
	} else {
		handle_motion_prethreshold(seat);
	}
	animation_set_type(ANIMATION_DISABLED);
	transaction_commit_dirty();
}

static void finalize_move(struct sway_seat *seat) {
	struct seatop_move_tiling_event *e = seat->seatop_data;

	if (!e->target_node) {
		seatop_begin_default(seat);
		return;
	}

	struct sway_container *con = e->con;
	struct sway_node *target_node = e->target_node;
	struct sway_workspace *workspace = target_node->type == N_WORKSPACE ?
		target_node->sway_workspace : target_node->sway_container->pending.workspace;
	enum wlr_edges edge = e->target_edge;

	if (target_node->type == N_WORKSPACE) {
		// Moving container into workspace
		layout_drag_container_to_workspace(con, workspace);
	} else {
		layout_drag_container_to_container(con, target_node->sway_container, workspace, edge);
	}
	ipc_event_window(con, "move");
	seat_set_focus(seat, &con->node);
	arrange_workspace(workspace);
	animation_set_type(ANIMATION_WINDOW_MOVE);
	transaction_commit_dirty();
	seatop_begin_default(seat);
}

static void handle_button(struct sway_seat *seat, uint32_t time_msec,
		struct wlr_input_device *device, uint32_t button,
		enum wl_pointer_button_state state) {
	if (seat->cursor->pressed_button_count == 0) {
		finalize_move(seat);
	}
}

static void handle_tablet_tool_tip(struct sway_seat *seat,
		struct sway_tablet_tool *tool, uint32_t time_msec,
		enum wlr_tablet_tool_tip_state state) {
	if (state == WLR_TABLET_TOOL_TIP_UP) {
		finalize_move(seat);
	}
}

static void handle_unref(struct sway_seat *seat, struct sway_container *con) {
	struct seatop_move_tiling_event *e = seat->seatop_data;
	if (e->target_node == &con->node) { // Drop target
		set_target_node(e, NULL);
	}
	if (e->con == con) { // The container being moved
		seatop_begin_default(seat);
	}
}

static const struct sway_seatop_impl seatop_impl = {
	.button = handle_button,
	.pointer_motion = handle_pointer_motion,
	.tablet_tool_tip = handle_tablet_tool_tip,
	.unref = handle_unref,
	.end = handle_end,
};

void seatop_begin_move_tiling_threshold(struct sway_seat *seat,
		struct sway_container *con) {
	seatop_end(seat);

	struct seatop_move_tiling_event *e =
		calloc(1, sizeof(struct seatop_move_tiling_event));
	if (!e) {
		return;
	}

	const float *indicator = config->border_colors.focused.indicator;
	float color[4] = {
		indicator[0] * .5,
		indicator[1] * .5,
		indicator[2] * .5,
		indicator[3] * .5,
	};
	e->indicator_rect = wlr_scene_rect_create(seat->scene_tree, 0, 0, color);
	if (!e->indicator_rect) {
		free(e);
		return;
	}

	e->con = con;
	e->ref_lx = seat->cursor->cursor->x;
	e->ref_ly = seat->cursor->cursor->y;
	wl_list_init(&e->target_node_destroy.link);
	e->target_node_destroy.notify = handle_target_node_destroy;

	seat->seatop_impl = &seatop_impl;
	seat->seatop_data = e;

	container_raise_floating(con);
	animation_set_type(ANIMATION_DISABLED);
	transaction_commit_dirty();
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
}

void seatop_begin_move_tiling(struct sway_seat *seat,
		struct sway_container *con) {
	seatop_begin_move_tiling_threshold(seat, con);
	struct seatop_move_tiling_event *e = seat->seatop_data;
	if (e) {
		e->threshold_reached = true;
		cursor_set_image(seat->cursor, "grab", NULL);
	}
}
