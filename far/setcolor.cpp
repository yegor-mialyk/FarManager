﻿/*
setcolor.cpp

Установка фаровских цветов
*/
/*
Copyright © 1996 Eugene Roshal
Copyright © 2000 Far Group
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// BUGBUG
#include "platform.headers.hpp"

// Self:
#include "setcolor.hpp"

// Internal:
#include "farcolor.hpp"
#include "color_picker.hpp"
#include "colormix.hpp"
#include "vmenu.hpp"
#include "vmenu2.hpp"
#include "filepanels.hpp"
#include "ctrlobj.hpp"
#include "scrbuf.hpp"
#include "panel.hpp"
#include "config.hpp"
#include "lang.hpp"
#include "manager.hpp"
#include "message.hpp"
#include "global.hpp"
#include "lockscrn.hpp"
#include "FarDlgBuilder.hpp"
#include "pathmix.hpp"
#include "exception.hpp"
#include "log.hpp"
#include "tinyxml.hpp"

// Platform:

// Common:
#include "common/2d/point.hpp"

// External:
#include "format.hpp"

#include <share.h>

//----------------------------------------------------------------------------

static void ChangeColor(PaletteColors PaletteIndex, FarColor const* const BottomColor)
{
	auto NewColor = colors::PaletteColorToFarColor(PaletteIndex);

	auto Reset = false;

	if (!GetColorDialog(NewColor, false, BottomColor, &Reset) && !Reset)
		return;

	if (Reset)
		NewColor = Global->Opt->Palette.Default(PaletteIndex);

	Global->Opt->Palette.Set(PaletteIndex, { &NewColor, 1 });

	{
		SCOPED_ACTION(LockScreen);

		Global->CtrlObject->Cp()->LeftPanel()->Update(UPDATE_KEEP_SELECTION);
		Global->CtrlObject->Cp()->LeftPanel()->Redraw();
		Global->CtrlObject->Cp()->RightPanel()->Update(UPDATE_KEEP_SELECTION);
		Global->CtrlObject->Cp()->RightPanel()->Redraw();

		Global->WindowManager->ResizeAllWindows(); // рефрешим
		Global->WindowManager->PluginCommit(); // коммитим.
	}

	Global->WindowManager->PluginCommit(); // коммитим.
}

enum list_mode
{
	lm_list_normal,
	lm_list_warning,
	lm_combo_normal,
	lm_combo_warning,

	list_modes_count
};

struct color_item;

class color_item_span
{
public:
	constexpr color_item_span() = default;

	template<size_t N>
	explicit(false) constexpr color_item_span(color_item const (&Items)[N]):
		data(Items),
		size(N)
	{
	}

	constexpr bool empty() const
	{
		return !size;
	}

	constexpr operator std::span<color_item const>() const;

private:
	color_item const* data{};
	size_t size{};
};

struct color_item
{
	lng LngId;
	PaletteColors Color;
	// libc++ decided to follow the standard literally and prohibit incomplete types in spans :(
	color_item_span SubColor;
	std::initializer_list<PaletteColors> BottomColorIndices;
};

constexpr color_item_span::operator std::span<color_item const>() const
{
	return { data, data + size };
}

static void SetItemColors(std::span<const color_item> const Items, point Position = {})
{
	const auto ItemsMenu = VMenu2::create(msg(lng::MSetColorItemsTitle), {});

	for (const auto& i: Items)
	{
		ItemsMenu->AddItem(msg(i.LngId));
	}

	ItemsMenu->SetPosition({ Position.x += 10, Position.y += 5, 0, 0 });
	ItemsMenu->SetMenuFlags(VMENU_WRAPMODE);
	ItemsMenu->RunEx([&](int Msg, void *param)
	{
		const auto ItemsCode = std::bit_cast<intptr_t>(param);
		if (Msg != DN_CLOSE || ItemsCode < 0)
			return 0;

		const auto& CurrentItems = Items[ItemsCode];

		if (!CurrentItems.SubColor.empty())
		{
			SetItemColors(CurrentItems.SubColor, Position);
		}
		else
		{
			std::optional<FarColor> BakedBottomColor;

			for (const auto& i: CurrentItems.BottomColorIndices)
			{
				const auto BottomColor = colors::PaletteColorToFarColor(i);
				if (!BakedBottomColor)
					BakedBottomColor = BottomColor;
				else
					*BakedBottomColor = colors::merge(BottomColor, *BakedBottomColor);
			}

			ChangeColor(CurrentItems.Color, BakedBottomColor? &*BakedBottomColor : nullptr);
		}

		return 1;
	});
}

static void ConfigurePalette()
{
	DialogBuilder Builder(lng::MColorsPalette, {});
	Builder.AddCheckbox(lng::MColorsClassicPalette, Global->Opt->SetPalette);
	Builder.AddOKCancel();
	Builder.ShowDialog();
}

static void list_themes(function_ref<void(string_view, string_view)> const Processor)
{
	const auto ThemesPath = path::join(Global->g_strFarPath, L"Addons\\Colors\\Interface"sv);
	for (const auto& i: os::fs::enum_files(path::join(ThemesPath, L"*.farconfig"sv)))
	{
		Processor(name_ext(i.FileName).first, path::join(ThemesPath, i.FileName));
	}
}

static void load_theme(string_view const File, tinyxml::XMLDocument& Document)
{
	const file_ptr XmlFile(_wfsopen(nt_path(File).c_str(), L"rb", _SH_DENYWR));
	if (!XmlFile)
		throw far_exception(far::format(L"Error opening file \"{}\": {}"sv, File, os::format_errno(errno)));

	if (const auto LoadResult = Document.LoadFile(XmlFile.get()); LoadResult != tinyxml::XML_SUCCESS)
		throw far_exception(encoding::utf8::get_chars(Document.ErrorIDToName(LoadResult)), false);
}

static void apply_external_theme(string_view const File)
{
	tinyxml::XMLDocument Document;
	load_theme(File, Document);
	tinyxml::XMLHandle Root(Document.RootElement());

	unordered_string_map<tinyxml::XMLElement const*> ColorEntries;

	for (const auto& e: xml::enum_nodes(Root.FirstChildElement("colors"), "object"))
	{
		if (const auto Name = e.Attribute("name"))
			ColorEntries.emplace(encoding::utf8::get_chars(Name), &e);
	}

	Global->Opt->Palette.LoadTheme([&](string_view const Key, FarColor const& Default)
	{
		const auto Iterator = ColorEntries.find(Key);
		if (Iterator == ColorEntries.cend())
			return Default;

		return deserialize_color([&](char const* const AttributeName){ return Iterator->second->Attribute(AttributeName); }, Default);
	});
}

static void try_apply_external_theme(string const& File)
{
	try
	{
		apply_external_theme(File);
	}
	catch (std::exception const& e)
	{
		LOGERROR(L"{}"sv, e);

		Message(MSG_WARNING, e,
			msg(lng::MError),
			{
				File,
			},
			{ lng::MOk });
	}
}

static void choose_theme()
{
	const auto ThemesMenu = VMenu2::create(msg(lng::MColorThemes), {});

	const auto DefaultIndexId = static_cast<int>(ThemesMenu->size());
	ThemesMenu->AddItem(msg(lng::MDefaultTheme));
	const auto DefaultRGBId = static_cast<int>(ThemesMenu->size());
	ThemesMenu->AddItem(msg(lng::MDefaultThemeRGB));

	list_themes([&, SeparatorAdded = false](string_view const Name, string_view const FullPath) mutable
	{
		if (!SeparatorAdded)
		{
			ThemesMenu->AddItem(MenuItemEx{ {}, LIF_SEPARATOR });
			SeparatorAdded = true;
		}

		MenuItemEx Item(Name);
		Item.ComplexUserData = string(FullPath);
		ThemesMenu->AddItem(Item);
	});

	ThemesMenu->SetPosition({ 2, 1, 0, 0 });
	ThemesMenu->SetMenuFlags(VMENU_WRAPMODE);
	ThemesMenu->SetHelp(L"ColorThemes"sv);

	const auto ThemeCode = ThemesMenu->Run();
	if (ThemeCode < 0)
		return;

	if (ThemeCode == DefaultIndexId)
		Global->Opt->Palette.ResetToDefaultIndex();
	else if (ThemeCode == DefaultRGBId)
		Global->Opt->Palette.ResetToDefaultRGB();
	else
		try_apply_external_theme(std::any_cast<string>(ThemesMenu->at(ThemeCode).ComplexUserData));
}

void SetColors()
{
	// NOT constexpr, wrong codegen in color_item::BottomColorIndices
	static const color_item
	PanelItems[]
	{
		{ lng::MSetColorPanelNormal,                COL_PANELTEXT },
		{ lng::MSetColorPanelSelected,              COL_PANELSELECTEDTEXT, {}, { COL_PANELTEXT } },
		{ lng::MSetColorPanelHighlightedText,       COL_PANELHIGHLIGHTTEXT },
		{ lng::MSetColorPanelHighlightedInfo,       COL_PANELINFOTEXT },
		{ lng::MSetColorPanelDragging,              COL_PANELDRAGTEXT },
		{ lng::MSetColorPanelBox,                   COL_PANELBOX },
		{ lng::MSetColorPanelNormalCursor,          COL_PANELCURSOR, {}, { COL_PANELTEXT } },
		{ lng::MSetColorPanelSelectedCursor,        COL_PANELSELECTEDCURSOR, {}, { COL_PANELCURSOR, COL_PANELSELECTEDTEXT, COL_PANELTEXT } },
		{ lng::MSetColorPanelNormalTitle,           COL_PANELTITLE },
		{ lng::MSetColorPanelSelectedTitle,         COL_PANELSELECTEDTITLE },
		{ lng::MSetColorPanelColumnTitle,           COL_PANELCOLUMNTITLE },
		{ lng::MSetColorPanelTotalInfo,             COL_PANELTOTALINFO },
		{ lng::MSetColorPanelSelectedInfo,          COL_PANELSELECTEDINFO },
		{ lng::MSetColorPanelScrollbar,             COL_PANELSCROLLBAR },
		{ lng::MSetColorPanelScreensNumber,         COL_PANELSCREENSNUMBER },
	},

	ListItemsNormal[]
	{
		{ lng::MSetColorDialogListText,                          COL_DIALOGLISTTEXT },
		{ lng::MSetColorDialogListHighLight,                     COL_DIALOGLISTHIGHLIGHT },
		{ lng::MSetColorDialogListSelectedText,                  COL_DIALOGLISTSELECTEDTEXT },
		{ lng::MSetColorDialogListSelectedHighLight,             COL_DIALOGLISTSELECTEDHIGHLIGHT },
		{ lng::MSetColorDialogListDisabled,                      COL_DIALOGLISTDISABLED },
		{ lng::MSetColorDialogListBox,                           COL_DIALOGLISTBOX },
		{ lng::MSetColorDialogListTitle,                         COL_DIALOGLISTTITLE },
		{ lng::MSetColorDialogListScrollBar,                     COL_DIALOGLISTSCROLLBAR },
		{ lng::MSetColorDialogListArrows,                        COL_DIALOGLISTARROWS },
		{ lng::MSetColorDialogListArrowsSelected,                COL_DIALOGLISTARROWSSELECTED },
		{ lng::MSetColorDialogListArrowsDisabled,                COL_DIALOGLISTARROWSDISABLED },
		{ lng::MSetColorDialogListGrayed,                        COL_DIALOGLISTGRAY },
		{ lng::MSetColorDialogSelectedListGrayed,                COL_DIALOGLISTSELECTEDGRAYTEXT },
	},

	ListItemsWarn[]
	{
		{ lng::MSetColorDialogListText,                          COL_WARNDIALOGLISTTEXT },
		{ lng::MSetColorDialogListHighLight,                     COL_WARNDIALOGLISTHIGHLIGHT },
		{ lng::MSetColorDialogListSelectedText,                  COL_WARNDIALOGLISTSELECTEDTEXT },
		{ lng::MSetColorDialogListSelectedHighLight,             COL_WARNDIALOGLISTSELECTEDHIGHLIGHT },
		{ lng::MSetColorDialogListDisabled,                      COL_WARNDIALOGLISTDISABLED },
		{ lng::MSetColorDialogListBox,                           COL_WARNDIALOGLISTBOX },
		{ lng::MSetColorDialogListTitle,                         COL_WARNDIALOGLISTTITLE },
		{ lng::MSetColorDialogListScrollBar,                     COL_WARNDIALOGLISTSCROLLBAR },
		{ lng::MSetColorDialogListArrows,                        COL_WARNDIALOGLISTARROWS },
		{ lng::MSetColorDialogListArrowsSelected,                COL_WARNDIALOGLISTARROWSSELECTED },
		{ lng::MSetColorDialogListArrowsDisabled,                COL_WARNDIALOGLISTARROWSDISABLED },
		{ lng::MSetColorDialogListGrayed,                        COL_WARNDIALOGLISTGRAY },
		{ lng::MSetColorDialogSelectedListGrayed,                COL_WARNDIALOGLISTSELECTEDGRAYTEXT },
	},

	ComboItemsNormal[]
	{
		{ lng::MSetColorDialogListText,                          COL_DIALOGCOMBOTEXT },
		{ lng::MSetColorDialogListHighLight,                     COL_DIALOGCOMBOHIGHLIGHT },
		{ lng::MSetColorDialogListSelectedText,                  COL_DIALOGCOMBOSELECTEDTEXT },
		{ lng::MSetColorDialogListSelectedHighLight,             COL_DIALOGCOMBOSELECTEDHIGHLIGHT },
		{ lng::MSetColorDialogListDisabled,                      COL_DIALOGCOMBODISABLED },
		{ lng::MSetColorDialogListBox,                           COL_DIALOGCOMBOBOX },
		{ lng::MSetColorDialogListTitle,                         COL_DIALOGCOMBOTITLE },
		{ lng::MSetColorDialogListScrollBar,                     COL_DIALOGCOMBOSCROLLBAR },
		{ lng::MSetColorDialogListArrows,                        COL_DIALOGCOMBOARROWS },
		{ lng::MSetColorDialogListArrowsSelected,                COL_DIALOGCOMBOARROWSSELECTED },
		{ lng::MSetColorDialogListArrowsDisabled,                COL_DIALOGCOMBOARROWSDISABLED },
		{ lng::MSetColorDialogListGrayed,                        COL_DIALOGCOMBOGRAY },
		{ lng::MSetColorDialogSelectedListGrayed,                COL_DIALOGCOMBOSELECTEDGRAYTEXT },
	},

	ComboItemsWarn[]
	{
		{ lng::MSetColorDialogListText,                          COL_WARNDIALOGCOMBOTEXT },
		{ lng::MSetColorDialogListHighLight,                     COL_WARNDIALOGCOMBOHIGHLIGHT },
		{ lng::MSetColorDialogListSelectedText,                  COL_WARNDIALOGCOMBOSELECTEDTEXT },
		{ lng::MSetColorDialogListSelectedHighLight,             COL_WARNDIALOGCOMBOSELECTEDHIGHLIGHT },
		{ lng::MSetColorDialogListDisabled,                      COL_WARNDIALOGCOMBODISABLED },
		{ lng::MSetColorDialogListBox,                           COL_WARNDIALOGCOMBOBOX },
		{ lng::MSetColorDialogListTitle,                         COL_WARNDIALOGCOMBOTITLE },
		{ lng::MSetColorDialogListScrollBar,                     COL_WARNDIALOGCOMBOSCROLLBAR },
		{ lng::MSetColorDialogListArrows,                        COL_WARNDIALOGCOMBOARROWS },
		{ lng::MSetColorDialogListArrowsSelected,                COL_WARNDIALOGCOMBOARROWSSELECTED },
		{ lng::MSetColorDialogListArrowsDisabled,                COL_WARNDIALOGCOMBOARROWSDISABLED },
		{ lng::MSetColorDialogListGrayed,                        COL_WARNDIALOGCOMBOGRAY },
		{ lng::MSetColorDialogSelectedListGrayed,                COL_WARNDIALOGCOMBOSELECTEDGRAYTEXT },
	},

	DialogItems[]
	{
		{ lng::MSetColorDialogNormal,                                   COL_DIALOGTEXT },
		{ lng::MSetColorDialogHighlighted,                              COL_DIALOGHIGHLIGHTTEXT },
		{ lng::MSetColorDialogDisabled,                                 COL_DIALOGDISABLED },
		{ lng::MSetColorDialogBox,                                      COL_DIALOGBOX },
		{ lng::MSetColorDialogBoxTitle,                                 COL_DIALOGBOXTITLE },
		{ lng::MSetColorDialogHighlightedBoxTitle,                      COL_DIALOGHIGHLIGHTBOXTITLE },
		{ lng::MSetColorDialogTextInput,                                COL_DIALOGEDIT },
		{ lng::MSetColorDialogUnchangedTextInput,                       COL_DIALOGEDITUNCHANGED },
		{ lng::MSetColorDialogSelectedTextInput,                        COL_DIALOGEDITSELECTED },
		{ lng::MSetColorDialogEditDisabled,                             COL_DIALOGEDITDISABLED },
		{ lng::MSetColorDialogButtons,                                  COL_DIALOGBUTTON },
		{ lng::MSetColorDialogSelectedButtons,                          COL_DIALOGSELECTEDBUTTON },
		{ lng::MSetColorDialogHighlightedButtons,                       COL_DIALOGHIGHLIGHTBUTTON },
		{ lng::MSetColorDialogSelectedHighlightedButtons,               COL_DIALOGHIGHLIGHTSELECTEDBUTTON },
		{ lng::MSetColorDialogDefaultButton,                            COL_DIALOGDEFAULTBUTTON },
		{ lng::MSetColorDialogSelectedDefaultButton,                    COL_DIALOGSELECTEDDEFAULTBUTTON },
		{ lng::MSetColorDialogHighlightedDefaultButton,                 COL_DIALOGHIGHLIGHTDEFAULTBUTTON },
		{ lng::MSetColorDialogSelectedHighlightedDefaultButton,         COL_DIALOGHIGHLIGHTSELECTEDDEFAULTBUTTON },
		{ lng::MSetColorDialogListBoxControl,                           {}, ListItemsNormal },
		{ lng::MSetColorDialogComboBoxControl,                          {}, ComboItemsNormal },
	},

	WarnDialogItems[]
	{
		{ lng::MSetColorDialogNormal,                                   COL_WARNDIALOGTEXT },
		{ lng::MSetColorDialogHighlighted,                              COL_WARNDIALOGHIGHLIGHTTEXT },
		{ lng::MSetColorDialogDisabled,                                 COL_WARNDIALOGDISABLED },
		{ lng::MSetColorDialogBox,                                      COL_WARNDIALOGBOX },
		{ lng::MSetColorDialogBoxTitle,                                 COL_WARNDIALOGBOXTITLE },
		{ lng::MSetColorDialogHighlightedBoxTitle,                      COL_WARNDIALOGHIGHLIGHTBOXTITLE },
		{ lng::MSetColorDialogTextInput,                                COL_WARNDIALOGEDIT },
		{ lng::MSetColorDialogUnchangedTextInput,                       COL_WARNDIALOGEDITUNCHANGED },
		{ lng::MSetColorDialogSelectedTextInput,                        COL_WARNDIALOGEDITSELECTED },
		{ lng::MSetColorDialogEditDisabled,                             COL_WARNDIALOGEDITDISABLED },
		{ lng::MSetColorDialogButtons,                                  COL_WARNDIALOGBUTTON },
		{ lng::MSetColorDialogSelectedButtons,                          COL_WARNDIALOGSELECTEDBUTTON },
		{ lng::MSetColorDialogHighlightedButtons,                       COL_WARNDIALOGHIGHLIGHTBUTTON },
		{ lng::MSetColorDialogSelectedHighlightedButtons,               COL_WARNDIALOGHIGHLIGHTSELECTEDBUTTON },
		{ lng::MSetColorDialogDefaultButton,                            COL_WARNDIALOGDEFAULTBUTTON },
		{ lng::MSetColorDialogSelectedDefaultButton,                    COL_WARNDIALOGSELECTEDDEFAULTBUTTON },
		{ lng::MSetColorDialogHighlightedDefaultButton,                 COL_WARNDIALOGHIGHLIGHTDEFAULTBUTTON },
		{ lng::MSetColorDialogSelectedHighlightedDefaultButton,         COL_WARNDIALOGHIGHLIGHTSELECTEDDEFAULTBUTTON },
		{ lng::MSetColorDialogListBoxControl,                           {}, ListItemsWarn },
		{ lng::MSetColorDialogComboBoxControl,                          {}, ComboItemsWarn },
	},

	MenuItems[]
	{
		{ lng::MSetColorMenuNormal,                   COL_MENUTEXT },
		{ lng::MSetColorMenuSelected,                 COL_MENUSELECTEDTEXT },
		{ lng::MSetColorMenuHighlighted,              COL_MENUHIGHLIGHT },
		{ lng::MSetColorMenuSelectedHighlighted,      COL_MENUSELECTEDHIGHLIGHT },
		{ lng::MSetColorMenuDisabled,                 COL_MENUDISABLEDTEXT },
		{ lng::MSetColorMenuBox,                      COL_MENUBOX },
		{ lng::MSetColorMenuTitle,                    COL_MENUTITLE },
		{ lng::MSetColorMenuScrollBar,                COL_MENUSCROLLBAR },
		{ lng::MSetColorMenuArrows,                   COL_MENUARROWS },
		{ lng::MSetColorMenuArrowsSelected,           COL_MENUARROWSSELECTED },
		{ lng::MSetColorMenuArrowsDisabled,           COL_MENUARROWSDISABLED },
		{ lng::MSetColorMenuGrayed,                   COL_MENUGRAYTEXT },
		{ lng::MSetColorMenuSelectedGrayed,           COL_MENUSELECTEDGRAYTEXT },
	},

	HMenuItems[]
	{
		{ lng::MSetColorHMenuNormal,                  COL_HMENUTEXT },
		{ lng::MSetColorHMenuSelected,                COL_HMENUSELECTEDTEXT },
		{ lng::MSetColorHMenuHighlighted,             COL_HMENUHIGHLIGHT },
		{ lng::MSetColorHMenuSelectedHighlighted,     COL_HMENUSELECTEDHIGHLIGHT },
	},

	KeyBarItems[]
	{
		{ lng::MSetColorKeyBarNumbers,                COL_KEYBARNUM },
		{ lng::MSetColorKeyBarNames,                  COL_KEYBARTEXT },
		{ lng::MSetColorKeyBarBackground,             COL_KEYBARBACKGROUND },
	},

	CommandLineItems[]
	{
		{ lng::MSetColorCommandLineNormal,            COL_COMMANDLINE },
		{ lng::MSetColorCommandLineSelected,          COL_COMMANDLINESELECTED },
		{ lng::MSetColorCommandLinePrefix,            COL_COMMANDLINEPREFIX },
		{ lng::MSetColorCommandLineUserScreen,        COL_COMMANDLINEUSERSCREEN },
	},

	ClockItems[]
	{
		{ lng::MSetColorClockNormal,                  COL_CLOCK },
		{ lng::MSetColorClockNormalEditor,            COL_EDITORCLOCK },
		{ lng::MSetColorClockNormalViewer,            COL_VIEWERCLOCK },
	},

	ViewerItems[]
	{
		{ lng::MSetColorViewerNormal,                 COL_VIEWERTEXT },
		{ lng::MSetColorViewerSelected,               COL_VIEWERSELECTEDTEXT },
		{ lng::MSetColorViewerStatus,                 COL_VIEWERSTATUS },
		{ lng::MSetColorViewerArrows,                 COL_VIEWERARROWS },
		{ lng::MSetColorViewerScrollbar,              COL_VIEWERSCROLLBAR },
	},

	EditorItems[]
	{
		{ lng::MSetColorEditorNormal,                 COL_EDITORTEXT },
		{ lng::MSetColorEditorSelected,               COL_EDITORSELECTEDTEXT, {}, { COL_EDITORTEXT } },
		{ lng::MSetColorEditorStatus,                 COL_EDITORSTATUS },
		{ lng::MSetColorEditorScrollbar,              COL_EDITORSCROLLBAR },
	},

	HelpItems[]
	{
		{ lng::MSetColorHelpNormal,                   COL_HELPTEXT },
		{ lng::MSetColorHelpHighlighted,              COL_HELPHIGHLIGHTTEXT },
		{ lng::MSetColorHelpReference,                COL_HELPTOPIC },
		{ lng::MSetColorHelpSelectedReference,        COL_HELPSELECTEDTOPIC },
		{ lng::MSetColorHelpBox,                      COL_HELPBOX },
		{ lng::MSetColorHelpBoxTitle,                 COL_HELPBOXTITLE },
		{ lng::MSetColorHelpScrollbar,                COL_HELPSCROLLBAR },
	};

	{
		static constexpr struct
		{
			lng MenuId;
			std::span<const color_item> Subitems;
		}
		Groups[]
		{
			{ lng::MSetColorPanel,       PanelItems },
			{ lng::MSetColorDialog,      DialogItems },
			{ lng::MSetColorWarning,     WarnDialogItems },
			{ lng::MSetColorMenu,        MenuItems },
			{ lng::MSetColorHMenu,       HMenuItems },
			{ lng::MSetColorKeyBar,      KeyBarItems },
			{ lng::MSetColorCommandLine, CommandLineItems },
			{ lng::MSetColorClock,       ClockItems },
			{ lng::MSetColorViewer,      ViewerItems },
			{ lng::MSetColorEditor,      EditorItems },
			{ lng::MSetColorHelp,        HelpItems },
		};

		const auto GroupsMenu = VMenu2::create(msg(lng::MSetColorGroupsTitle), {});

		for (const auto& i: Groups)
		{
			GroupsMenu->AddItem(msg(i.MenuId));
		}

		{
			MenuItemEx tmp;
			tmp.Flags = LIF_SEPARATOR;
			GroupsMenu->AddItem(tmp);
		}

		const auto ThemesId = static_cast<int>(GroupsMenu->size());
		GroupsMenu->AddItem(msg(lng::MColorThemes));
		GroupsMenu->SetHelp(L"ColorGroups"sv);

		{
			MenuItemEx tmp;
			tmp.Flags = LIF_SEPARATOR;
			GroupsMenu->AddItem(tmp);
		}

		const auto PaletteId = static_cast<int>(GroupsMenu->size());
		GroupsMenu->AddItem(msg(lng::MColorsPalette));

		GroupsMenu->SetPosition({ 2, 1, 0, 0 });
		GroupsMenu->SetMenuFlags(VMENU_WRAPMODE);
		const auto GroupsCode=GroupsMenu->RunEx([&](int Msg, void *param)
		{
			const auto ItemsCode = std::bit_cast<intptr_t>(param);
			if (Msg != DN_CLOSE || ItemsCode < 0 || static_cast<size_t>(ItemsCode) >= std::size(Groups))
				return 0;
			SetItemColors(Groups[ItemsCode].Subitems);
			return 1;
		});

		if (GroupsCode == ThemesId)
		{
			choose_theme();
		}
		else if (GroupsCode == PaletteId)
		{
			ConfigurePalette();
		}
	}
	Global->CtrlObject->Cp()->SetScreenPosition();
	Global->CtrlObject->Cp()->Redraw();
}
