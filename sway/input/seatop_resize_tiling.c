#include <math.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/edges.h>
#include "sway/commands.h"
#include "sway/desktop/transaction.h"
#include "sway/desktop/animation.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"

struct seatop_resize_tiling_event {
	struct sway_container *con;    // container to resize
	struct sway_workspace *workspace;

	enum wlr_edges edge;
	enum wlr_edges edge_x, edge_y;
	double ref_lx, ref_ly;         // cursor's x/y at start of op
	double con_orig_width;       // width of the container at start
	double con_orig_height;      // height of the container at start
	double min_width, max_width;
	double min_height, max_height;
	bool dirty;                    // pending motion not applied to the layout yet
	bool moved;                    // the pointer moved at least once
};

// Start/stop tracking an interactive resize in the workspace layout. While
// active, transaction.c keeps the edge opposite to the dragged one fixed, so
// the window follows the cursor instead of being re-centered/re-aligned on
// every motion. The normal auto alignment is restored when the drag ends.
static void resize_state_begin(struct sway_container *con, enum wlr_edges edge) {
	struct sway_workspace *workspace = con->pending.workspace;
	if (!workspace) {
		return;
	}
	struct sway_container *top = con;
	while (top->pending.parent) {
		top = top->pending.parent;
	}
	struct sway_scroller *scroller = &workspace->layout;
	scroller->resize.active = true;
	scroller->resize.container = con;
	scroller->resize.top = top;
	scroller->resize.edge = edge;
	scroller->resize.anchor_x = (edge & WLR_EDGE_LEFT) ?
		top->pending.x + top->pending.width : top->pending.x;
	scroller->resize.anchor_y = (edge & WLR_EDGE_TOP) ?
		top->pending.y + top->pending.height : top->pending.y;
	scroller->resize.leaf_anchor_x = (edge & WLR_EDGE_LEFT) ?
		con->pending.x + con->pending.width : con->pending.x;
	scroller->resize.leaf_anchor_y = (edge & WLR_EDGE_TOP) ?
		con->pending.y + con->pending.height : con->pending.y;
}

static void resize_state_clear(struct sway_workspace *workspace,
		struct sway_container *con) {
	if (!workspace) {
		return;
	}
	struct sway_scroller *scroller = &workspace->layout;
	if (con && scroller->resize.container != con) {
		return;
	}
	scroller->resize.active = false;
	scroller->resize.container = NULL;
	scroller->resize.top = NULL;
	scroller->resize.edge = WLR_EDGE_NONE;
}

// Anchor the dragged edge only for normal, non-fit layouts. Fit/overview
// modes re-arrange the whole workspace and keep their existing behavior.
static bool resize_anchoring_enabled(struct sway_workspace *workspace,
		struct sway_container *con) {
	if (!workspace || !workspace->layout.resize.active ||
			workspace->layout.resize.container != con) {
		return false;
	}
	if (layout_overview_mode(workspace) != OVERVIEW_DISABLED) {
		return false;
	}
	return layout_modifiers_get_fit(workspace) == FIT_NONE;
}

static void arrange_resized_container(struct sway_container *con) {
	enum sway_container_layout layout = layout_get_type(con->pending.workspace);
	struct sway_container *parent = con->pending.parent;
	struct sway_workspace *workspace = parent->pending.workspace;
	if (layout == L_HORIZ) {
		parent->pending.width = con->pending.width;
		parent->width_fraction = (parent->pending.width + 2.0 * workspace->gaps_inner) / workspace->width;
		for (int i = 0; i < parent->pending.children->length; ++i) {
			struct sway_container *child = parent->pending.children->items[i];
			child->pending.width = parent->pending.width;
			child->width_fraction = parent->width_fraction;
		}
		con->height_fraction = (con->pending.height + 2.0 * workspace->gaps_inner) / workspace->height;
	} else {
		parent->pending.height = con->pending.height;
		parent->height_fraction = (parent->pending.height + 2.0 * workspace->gaps_inner) / workspace->height;
		for (int i = 0; i < parent->pending.children->length; ++i) {
			struct sway_container *child = parent->pending.children->items[i];
			child->pending.height = parent->pending.height;
			child->height_fraction = parent->height_fraction;
		}
		con->width_fraction = (con->pending.width + 2.0 * workspace->gaps_inner) / workspace->width;
	}
	// Keep the edge opposite to the dragged one fixed while the resize is in
	// progress. RIGHT/BOTTOM drags keep x/y unchanged (the left/top edge is
	// already the anchor); LEFT/TOP drags move x/y so the right/bottom edge
	// stays where it was.
	struct sway_scroller *scroller = &workspace->layout;
	if (resize_anchoring_enabled(workspace, con)) {
		enum wlr_edges edge = scroller->resize.edge;
		if (layout == L_HORIZ) {
			if (edge & WLR_EDGE_LEFT) {
				parent->pending.x =
					scroller->resize.anchor_x - parent->pending.width;
				// All the children of a column share its x coordinate.
				for (int i = 0; i < parent->pending.children->length; ++i) {
					struct sway_container *child =
						parent->pending.children->items[i];
					child->pending.x = parent->pending.x;
				}
			}
			if (edge & WLR_EDGE_TOP) {
				con->pending.y =
					scroller->resize.leaf_anchor_y - con->pending.height;
			}
		} else {
			if (edge & WLR_EDGE_TOP) {
				parent->pending.y =
					scroller->resize.anchor_y - parent->pending.height;
				// All the children of a row share its y coordinate.
				for (int i = 0; i < parent->pending.children->length; ++i) {
					struct sway_container *child =
						parent->pending.children->items[i];
					child->pending.y = parent->pending.y;
				}
			}
			if (edge & WLR_EDGE_LEFT) {
				con->pending.x =
					scroller->resize.leaf_anchor_x - con->pending.width;
			}
		}
	}
	if (layout_modifiers_get_fit(workspace) != FIT_NONE) {
		arrange_workspace(workspace);
	}
	arrange_container(parent);
}

// Apply the latest pending size to the layout. The transaction is committed
// without waiting for the client to acknowledge the configure: the container
// is resized immediately and the client's buffer catches up asynchronously.
static void resize_flush(struct seatop_resize_tiling_event *e) {
	if (!e || !e->dirty) {
		return;
	}
	e->dirty = false;
	if (!e->con) {
		return;
	}
	arrange_resized_container(e->con);
	animation_set_type(ANIMATION_DISABLED);
	if (e->con->view) {
		transaction_commit_dirty_resize(e->con->view, true, false);
	} else {
		transaction_commit_dirty();
	}
}

static void handle_output_frame(struct sway_seat *seat,
		struct sway_output *output) {
	struct seatop_resize_tiling_event *e = seat->seatop_data;
	if (!e || !e->dirty) {
		return;
	}
	if (!e->workspace || e->workspace->output != output) {
		return;
	}
	resize_flush(e);
}

static void handle_button(struct sway_seat *seat, uint32_t time_msec,
		struct wlr_input_device *device, uint32_t button,
		enum wl_pointer_button_state state) {
	struct seatop_resize_tiling_event *e = seat->seatop_data;

	if (seat->cursor->pressed_button_count == 0) {
		e->dirty = false;
		if (e->con) {
			arrange_resized_container(e->con);
			container_set_resizing(e->con, false);
			arrange_workspace(e->workspace);
			layout_tiling_resize_callback(e->con);
			// The drag is over: clear the anchored state before committing so
			// the final transaction re-centers/re-aligns the container.
			resize_state_clear(e->workspace, e->con);
		}
		animation_set_type(ANIMATION_WINDOW_SIZE);
		if (e->moved && e->con && e->con->view) {
			// Send a final configure and wait for it, so the client's last
			// buffer matches the final container size.
			transaction_commit_dirty_resize(e->con->view, false, true);
		} else {
			transaction_commit_dirty();
		}
		seatop_begin_default(seat);
	}
}

static void handle_pointer_motion(struct sway_seat *seat, uint32_t time_msec) {
	struct seatop_resize_tiling_event *e = seat->seatop_data;
	int amount_x = 0;
	int amount_y = 0;
	int moved_x = seat->cursor->cursor->x - e->ref_lx;
	int moved_y = seat->cursor->cursor->y - e->ref_ly;

	if (e->edge_x) {
		if (e->edge_x & WLR_EDGE_LEFT) {
			amount_x = e->con_orig_width - moved_x;
		} else if (e->edge_x & WLR_EDGE_RIGHT) {
			amount_x = e->con_orig_width + moved_x;
		}
		e->con->pending.width = fmax(fmin(amount_x, e->max_width), e->min_width);
	}
	if (e->edge_y) {
		if (e->edge_y & WLR_EDGE_TOP) {
			amount_y = e->con_orig_height - moved_y;
		} else if (e->edge_y & WLR_EDGE_BOTTOM) {
			amount_y = e->con_orig_height + moved_y;
		}
		e->con->pending.height = fmax(fmin(amount_y, e->max_height), e->min_height);
	}
	e->dirty = true;
	e->moved = true;
	struct sway_output *output = e->workspace ? e->workspace->output : NULL;
	if (output && output->wlr_output->enabled) {
		// Coalesce motion events: apply the latest size once, right before
		// the next frame of the output the workspace is on.
		wlr_output_schedule_frame(output->wlr_output);
	} else {
		resize_flush(e);
	}
}

static void handle_unref(struct sway_seat *seat, struct sway_container *con) {
	struct seatop_resize_tiling_event *e = seat->seatop_data;
	if (e->con == con) {
		seatop_begin_default(seat);
	}
}

static void handle_end(struct sway_seat *seat) {
	struct seatop_resize_tiling_event *e = seat->seatop_data;
	if (e && e->workspace) {
		resize_state_clear(e->workspace, e->con);
	}
}

static const struct sway_seatop_impl seatop_impl = {
	.button = handle_button,
	.pointer_motion = handle_pointer_motion,
	.frame = handle_output_frame,
	.unref = handle_unref,
	.end = handle_end,
};

void seatop_begin_resize_tiling(struct sway_seat *seat,
		struct sway_container *con, enum wlr_edges edge) {
	seatop_end(seat);

	struct seatop_resize_tiling_event *e =
		calloc(1, sizeof(struct seatop_resize_tiling_event));
	if (!e) {
		return;
	}
	e->con = con;
	e->edge = edge;
	e->workspace = con->pending.workspace;

	e->ref_lx = seat->cursor->cursor->x;
	e->ref_ly = seat->cursor->cursor->y;

	container_set_resizing(e->con, true);

	if (edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
		e->edge_x = edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		e->con_orig_width = e->con->pending.width;
	}
	if (edge & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) {
		e->edge_y = edge & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
		e->con_orig_height = e->con->pending.height;
	}

	view_get_constraints(e->con->view, &e->min_width, &e->max_width, &e->min_height, &e->max_height);
	e->min_width = fmax(e->min_width, 1.0) + 2.0 * e->con->pending.border_thickness;
	e->max_width += 2.0 * e->con->pending.border_thickness;
	e->min_height = fmax(e->min_height, 1.0) + 2.0 * e->con->pending.border_thickness;
	e->max_height += 2.0 * e->con->pending.border_thickness;
	if (config->mouse_resize_tiling_limit) {
		struct sway_workspace *workspace = e->con->pending.workspace;
		if (workspace) {
			const int gaps = workspace->gaps_inner * 2;
			e->max_width = fmin(e->max_width, workspace->width - gaps);
			e->max_height = fmin(e->max_height, workspace->height - gaps);
		}
	}

	seat->seatop_impl = &seatop_impl;
	seat->seatop_data = e;

	resize_state_begin(con, edge);
	animation_set_type(ANIMATION_DISABLED);
	transaction_commit_dirty();
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
}
