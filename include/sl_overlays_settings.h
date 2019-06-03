#pragma once

#include <list>
#include <stdlib.h>
#include <string>

const std::string config_file_name = "settings.cfg";

class web_view_overlay_settings
{
	public:
	std::string url;
	int x;
	int y;
	int width;
	int height;

	
	web_view_overlay_settings();
};

class smg_settings
{
	public:
	const int settings_version;
	std::list<std::string> apps_names;
	std::list<web_view_overlay_settings> web_pages;

	int transparency; // o - 255
	bool use_color_key;
	int redraw_timeout; //ms

	void default_init();


	smg_settings();
};

extern std::shared_ptr<smg_settings> app_settings;
