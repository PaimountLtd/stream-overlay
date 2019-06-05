#pragma once
#include <mutex>
#include "stdafx.h"

enum class overlay_status : int
{
	creating = 1,
	source_ready,
	working,
	destroing
};

class overlay_window
{
	protected:
	RECT rect;
	bool manual_position;
	std::mutex rect_access;

	public:
	RECT get_rect();
	virtual bool set_rect(RECT& new_rect);
	virtual bool apply_new_rect(RECT& new_rect);
	virtual bool set_new_position(int x, int y);

	sl_window_capture_method use_method;
	overlay_status status;

	bool create_window_content_buffer();
	virtual bool paint_window_from_buffer(const void* image_array, size_t array_size, int width, int height);

	virtual void set_transparency(int transparency);
	virtual bool ready_to_create_overlay();
	HWND orig_handle;
	
	HBITMAP hbmp;
	HDC hdc;
	bool content_updated;

	HWND overlay_hwnd;

	int id;

	virtual ~overlay_window();
	overlay_window();

	virtual void clean_resources();
};
