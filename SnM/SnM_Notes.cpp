/******************************************************************************
/ SnM_Notes.cpp
/
/ Copyright (c) 2010 and later Jeffos
/
/
/ Permission is hereby granted, free of charge, to any person obtaining a copy
/ of this software and associated documentation files (the "Software"), to deal
/ in the Software without restriction, including without limitation the rights to
/ use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
/ of the Software, and to permit persons to whom the Software is furnished to
/ do so, subject to the following conditions:
/ 
/ The above copyright notice and this permission notice shall be included in all
/ copies or substantial portions of the Software.
/ 
/ THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
/ EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
/ OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
/ NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
/ HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
/ WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
/ FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
/ OTHER DEALINGS IN THE SOFTWARE.
/
******************************************************************************/

//JFB
// - if REAPER >= v4.55pre2 use MarkProjectDirty(), otherwise creates undo point for each key stroke (!) to fix:
//   * SaveExtensionConfig() that is not called when there is no proj mods but some notes have been entered..
//   * missing updates on project switches
// - clicking the empty area of the tcp does not remove focus (important for refresh)
// - undo does not restore caret position
// TODO?
// - take changed => title not updated
// - drag-drop text?
// - handle concurent item/project notes updates?

#include "stdafx.h"

#ifdef _WIN32
#pragma comment(lib, "comctl32.lib")
#include <winhttp.h>
#include <urlmon.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "urlmon.lib")
#else
#include <thread>
#endif

#include "version.h"

#include "SnM.h"
#include "SnM_Dlg.h"
#include "SnM_Misc.h"
#include "SnM_Notes.h"
#include "SnM_Project.h"
#include "SnM_Track.h"
#include "SnM_Util.h"
#include "SnM_Window.h"

#include "../Breeder/BR_ReaScript.h"
#include "../cfillion/cfillion.hpp"

#include <WDL/localize/localize.h>
#include <WDL/projectcontext.h>

#include <unordered_map>

#define NOTES_WND_ID				"SnMNotesHelp"
#define NOTES_INI_SEC				"Notes"
#define MAX_HELP_LENGTH				(64*1024) //JFB! instead of MAX_INI_SECTION (too large)
#define UPDATE_TIMER				1
#define ROLE_PROFILE_MSG_BASE		0xE000
#define ROLE_PROFILE_MSG_COUNT		128

enum {
  WRAP_MSG = 0xF001,
  SAVE_GLOBAL_NOTES_MSG,
  LINK_ACTOR_MSG,
  UNLINK_ACTOR_MSG,
  CHANGE_COLOR_MSG,
  COLORED_REGIONS_MSG,
  HIDE_REGIONS_MSG,
  HIDE_ACTOR_LIST_MSG,
  DISPLAY_ACTOR_PREFIX_MSG,
  IMPORT_ROLES_MSG,
  EXPORT_ROLES_MSG,
  ENABLE_ALL_MSG,
  DISABLE_ALL_MSG,
  COPY_MARKERS_MSG,
  COPY_MARKERS_WITH_FRAMES_MSG,
  TIME_FORMAT_NO_FRAMES_MSG,
  TIME_FORMAT_FRAMES_MSG,
  TIME_FORMAT_MS_MSG,
  COPY_ROLE_DISTRIBUTION_MSG,
  COPY_CURRENT_PHRASE_MSG,
  SELECT_LINKED_ACTOR_ROLES_MSG,
  HIGHLIGHT_ACTOR_ROLES_MSG,
  CLEAR_HIGHLIGHT_ROLES_MSG,
  DELETE_ROLE_MSG,
  DELETE_UNUSED_ROLES_MSG,
  CLEAR_ALL_ROLES_MSG,
  CREATE_SUBTITLE_ITEMS_TRACK_MSG,
  CHECK_RENOTES_UPDATES_MSG,
  AUTO_CHECK_UPDATES_MSG,
  TOGGLE_VIDEO_OVERLAY_MSG,
  TOGGLE_VIDEO_OVERLAY_ACTOR_MSG,
  VIDEO_SCALE_80_MSG,
  VIDEO_SCALE_100_MSG,
  VIDEO_SCALE_125_MSG,
  VIDEO_SCALE_150_MSG,
  VIDEO_POS_BOTTOM_MSG,
  VIDEO_POS_VERY_BOTTOM_MSG,
  VIDEO_POS_CENTER_MSG,
  VIDEO_POS_TOP_MSG,
  VIDEO_BG_NONE_MSG,
  VIDEO_BG_LIGHT_MSG,
  VIDEO_BG_MEDIUM_MSG,
  VIDEO_BG_DARK_MSG,
  VIDEO_COLOR_NONE_MSG,
  VIDEO_COLOR_ROLE_MSG,
  VIDEO_COLOR_ALL_MSG,
  OPEN_VIDEO_FX_MSG,
  REFRESH_VIDEO_OVERLAY_MSG,
  LAST_MSG // keep as last item!
};

enum {
  BTNID_LOCK=LAST_MSG,
  CMBID_TYPE,
  CMBID_REGION,
  TXTID_LABEL,
  BTNID_IMPORT_SUB,
  BTNID_CLEAR_SUBS,
  BTNID_VIDEO_OVERLAY,
  TXTID_BIG_NOTES
};

enum {
  REQUEST_REFRESH=0,
  NO_REFRESH
};

int GenerateActorColor(const char *actorName);
bool ImportAssFile(const char *_fn);
bool ImportSubRipFile(const char *_fn);
class SNM_Actor;
SNM_Actor *FindOrCreateActor(const char *name);


SNM_WindowManager<NotesWnd> g_notesWndMgr(NOTES_WND_ID);

SWSProjConfig<WDL_PtrList_DOD<SNM_TrackNotes> > g_SNM_TrackNotes;
SWSProjConfig<WDL_PtrList_DOD<SNM_RegionSubtitle> > g_pRegionSubs;
SWSProjConfig<WDL_PtrList_DOD<SNM_Actor> > g_actors;
SWSProjConfig<WDL_FastString> g_prjNotes; // extra project notes
// global notes #647, saved in <REAPER Resource Path>/SWS_GlobalNotes.txt 
// (no SWSProjConfig, one instance across all projects, i.e. global)
WDL_FastString g_globalNotes; 
bool g_globalNotesDirty = false;
bool g_coloredRegions = true;
bool g_displayActorInPrefix = true;
bool g_hideRegions = false;
bool g_hideActorList = false;
enum SnM_TimeFormat {
  SNM_TIME_FORMAT_NO_FRAMES = 0,
  SNM_TIME_FORMAT_FRAMES = 1,
  SNM_TIME_FORMAT_MS = 2
};
int g_copyTimeFormat = SNM_TIME_FORMAT_NO_FRAMES;
bool g_autoCheckUpdates = true;
static bool g_pendingRegionRecreation = false;
SWSProjConfig<WDL_PtrList_DOD<SNM_Actor> > g_importedRoleActors;
SWSProjConfig<WDL_FastString> g_activeRoleProfile;
SWSProjConfig<WDL_FastString> g_highlightedActor;
bool g_videoOverlayEnabled = false;
bool g_videoOverlayIncludeActor = true;
int g_videoOverlayColorMode = 1; // 0=None (White), 1=Role name only, 2=All replica
double g_videoOverlayScale = 1.0;
double g_videoOverlayPosY = 0.88;
double g_videoOverlayMaxWidth = 0.90;
double g_videoOverlayBgAlpha = 0.55;
static int s_gmemGeneration = 0;

static bool IsActorMatchingHighlight(const SNM_Actor *actor, const char *highlightName) {
  if (!actor || !highlightName || !*highlightName)
    return false;
  if (actor->HasLinkedActor() && !strcmp(actor->GetLinkedActorName(), highlightName))
    return true;
  if (!strcmp(actor->GetName(), highlightName))
    return true;
  return false;
}

struct RoleProfileMenuEntry {
  std::string name;
  std::string path;
};

static std::vector<RoleProfileMenuEntry> g_roleProfileMenuEntries;

static SNM_Actor *FindActorInList(WDL_PtrList_DOD<SNM_Actor> *actors, const char *name);
void CleanupStaleActors();
void ClearAllSubtitles();
void UpdateRegionColors();
void HideAllRegions();
void ShowAllRegions();
bool ExportRolesFile(const char *fn);
bool ImportRolesFile(const char *fn);
static bool LoadRolesFile(const char *fn);
static bool ImportRoleProfile(const char *name, const char *path);
static bool SaveActiveRoleProfile();
static void RefreshRoleProfileEntries();
void SyncSubtitleItemsTrack(bool createIfMissing);

int g_notesType = -1;
int g_prevNotesType = -1;
bool g_locked = true;
char g_lastText[MAX_HELP_LENGTH] = "";
char g_lastImportSubFn[SNM_MAX_PATH] = "";
char g_lastRolesFn[SNM_MAX_PATH] = "";
char g_notesBigFontName[64] = SNM_DYN_FONT_NAME;
bool g_wrapText=false;

// other vars for updates tracking
double g_lastMarkerPos = -1.0;
int g_lastMarkerRegionId = -1;
MediaItem* g_mediaItemNote = NULL;
MediaTrack* g_trNote = NULL;

// to distinguish internal marker/region updates from external ones
bool g_internalMkrRgnChange = false;
static int s_internalMkrRgnChangeDepth = 0;
struct InternalMkrRgnGuard {
  InternalMkrRgnGuard() { s_internalMkrRgnChangeDepth++; }
  ~InternalMkrRgnGuard() { if (s_internalMkrRgnChangeDepth > 0) s_internalMkrRgnChangeDepth--; }
};


SNM_TrackNotes *SNM_TrackNotes::find(MediaTrack *track)
{
	const GUID *guid = TrackToGuid(track);
	if (!guid)
		return nullptr;

	for (int i = 0; i < g_SNM_TrackNotes.Get()->GetSize(); ++i)
	{
		SNM_TrackNotes *notes = g_SNM_TrackNotes.Get()->Get(i);
		if (GuidsEqual(guid, notes->GetGUID()))
			return notes;
	}
	return nullptr;
}

///////////////////////////////////////////////////////////////////////////////
// NotesWnd
///////////////////////////////////////////////////////////////////////////////

// S&M windows lazy init: below's "" prevents registering the SWS' screenset callback
// (use the S&M one instead - already registered via SNM_WindowManager::Init())
NotesWnd::NotesWnd()
	: SWS_DockWnd(IDD_SNM_NOTES, __LOCALIZE("ReNotes","sws_DLG_152"), ""),
	m_edit { nullptr }, // NotesWnd may be constructed without being open
    m_settingText { false},
    m_actorListView{nullptr},
    m_contextMenuActor{nullptr}
{
	m_id.Set(NOTES_WND_ID);
	// Must call SWS_DockWnd::Init() to restore parameters and open the window if necessary.
	Init();
}

NotesWnd::~NotesWnd() = default;

void NotesWnd::OnInitDlg()
{
	SNM_SetIntConfigVar("projtimemode", 5);
	UpdateTimeline();

	m_edit = GetDlgItem(m_hwnd, IDC_EDIT1);
	HWND edit2 = GetDlgItem(m_hwnd, IDC_EDIT2);

	// don't passthrough input to the main window
	// https://forum.cockos.com/showthread.php?p=1208961
	SetWindowLongPtr(m_edit, GWLP_USERDATA, 0xdeadf00b);
	SetWindowLongPtr(edit2,  GWLP_USERDATA, 0xdeadf00b);

#ifdef __APPLE__
	// Prevent shortcuts in the menubar from triggering main window actions
	// bypassing the accelerator hook return value
	SWS_Mac_MakeDefaultWindowMenu(m_hwnd);

	// WS_VSCROLL makes SWELL use an NSTextView instead of NSTextField
	Mac_TextViewSetAllowsUndo(m_edit, true);
	Mac_TextViewSetAllowsUndo(edit2,  true);
#endif

	m_resize.init_item(IDC_EDIT1, 0.0, 0.0, 1.0, 1.0);
	m_resize.init_item(IDC_EDIT2, 0.0, 0.0, 1.0, 1.0);

#ifdef _WIN32
  DragAcceptFiles(m_hwnd, TRUE);
#endif

	SetWrapText(g_wrapText);

	LICE_CachedFont* font = SNM_GetThemeFont();

	m_vwnd_painter.SetGSC(WDL_STYLE_GetSysColor);
	m_parentVwnd.SetRealParent(m_hwnd);

	m_btnLock.SetID(BTNID_LOCK);
	m_parentVwnd.AddChild(&m_btnLock);

	m_cbType.SetID(CMBID_TYPE);
	m_cbType.SetFont(font);
	m_cbType.AddItem(__LOCALIZE("Заметки дорожки","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Заметки айтема","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Заметки проекта","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Доп. заметки проекта","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Глобальные заметки", "sws_DLG_152"));
	m_cbType.AddItem("<SEP>");
	m_cbType.AddItem(__LOCALIZE("Субтитры","sws_DLG_152"));
	m_parentVwnd.AddChild(&m_cbType);
	// ...the selected item is set through SetType() below

  m_cbRegion.SetID(CMBID_REGION);
  m_cbRegion.SetFont(font);
  m_parentVwnd.AddChild(&m_cbRegion);

	m_txtLabel.SetID(TXTID_LABEL);
	m_txtLabel.SetFont(font);
	m_parentVwnd.AddChild(&m_txtLabel);

	m_btnImportSub.SetID(BTNID_IMPORT_SUB);
	m_parentVwnd.AddChild(&m_btnImportSub);

  m_btnClearSubs.SetID(BTNID_CLEAR_SUBS);
	m_parentVwnd.AddChild(&m_btnClearSubs);

  m_btnVideoOverlay.SetID(BTNID_VIDEO_OVERLAY);
  m_parentVwnd.AddChild(&m_btnVideoOverlay);


#ifdef _WIN32
  HWND actorListHwnd = CreateWindowEx(0,
                                      WC_LISTVIEW,
                                      "",
                                      WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                      LVS_NOCOLUMNHEADER | LVS_NOSORTHEADER,
                                      0,
                                      0,
                                      150,
                                      200,
                                      m_hwnd,
                                      NULL,
                                      g_hInst,
                                      NULL);
  ListView_SetExtendedListViewStyle(actorListHwnd, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  HIMAGELIST hImgList = ImageList_Create(1, 34, ILC_COLOR, 1, 0);
  ListView_SetImageList(actorListHwnd, hImgList, LVSIL_SMALL);
#else
  HWND actorListHwnd = GetDlgItem(m_hwnd, IDC_LIST3);
#endif
  m_actorListView = new ActorListView(actorListHwnd, NULL);
  m_pLists.Add(m_actorListView);

	m_bigNotes.SetID(TXTID_BIG_NOTES);
	m_bigNotes.SetFontName(g_notesBigFontName);
	m_parentVwnd.AddChild(&m_bigNotes);

	g_prevNotesType = -1; // will force refresh
	memset(g_lastText, 0, sizeof(g_lastText));
	SetType(BOUNDED(g_notesType, 0, m_cbType.GetCount()-1)); // + Update()

/* see OnTimer()
	RegisterToMarkerRegionUpdates(&m_mkrRgnListener);
*/
	SetTimer(m_hwnd, UPDATE_TIMER, NOTES_UPDATE_FREQ, NULL);
}

void NotesWnd::OnDestroy() 
{
	KillTimer(m_hwnd, UPDATE_TIMER);
/* see OnTimer()
	UnregisterToMarkerRegionUpdates(&m_mkrRgnListener);
*/
#ifdef _WIN32
  if (m_actorListView) {
    HIMAGELIST hImgList = ListView_GetImageList(m_actorListView->GetHWND(), LVSIL_SMALL);
    if (hImgList)
      ImageList_Destroy(hImgList);
  }
#endif
	g_prevNotesType = -1;
	m_cbType.Empty();
  m_cbRegion.Empty();
  m_overlappingRegionIds.Resize(0);
	m_edit = nullptr;
  m_actorListView = nullptr;
}

// note: no diff with current type, init would fail otherwise
void NotesWnd::SetType(int _type)
{
	g_notesType = _type;
	m_cbType.SetCurSel2(g_notesType);
	SendMessage(m_hwnd, WM_SIZE, 0, 0); // to update the bottom of the GUI

	// force an initial refresh (when IDC_EDIT has the focus, re-enabling the timer 
	// isn't enough: Update() is skipped, see OnTimer() & IsActive()
	Update();
}

void NotesWnd::SetText(const char* _str, bool _addRN) {
	if (!_str)
		return;

	static char s_rnStr[sizeof(g_lastText)];
	if (_addRN) {
		GetStringWithRN(_str, s_rnStr, sizeof(s_rnStr));
		_str = &s_rnStr[0];
	}

	if(!strcmp(_str, g_lastText))
		return;

	lstrcpyn(g_lastText, _str, sizeof(g_lastText));
	m_settingText = true;
	SetWindowText(m_edit, g_lastText);
	m_settingText = false;
}

void NotesWnd::RefreshGUI()
{
	bool bHide = true;
	switch(g_notesType)
	{
		case SNM_NOTES_PROJECT:
		case SNM_NOTES_PROJECT_EXTRA:
		case SNM_NOTES_GLOBAL:
			bHide = false;
			break;
		case SNM_NOTES_ITEM:
			if (g_mediaItemNote)
				bHide = false;
			break;
		case SNM_NOTES_TRACK:
			if (g_trNote)
				bHide = false;
			break;
		case SNM_NOTES_RGN_SUB:
			if (g_hideRegions ? g_lastMarkerRegionId >= 0 : g_lastMarkerRegionId > 0)
				bHide = false;
			break;
	}
	ShowWindow(m_edit, bHide || g_locked ? SW_HIDE : SW_SHOW);
	m_parentVwnd.RequestRedraw(NULL); // the meat!
}

void NotesWnd::SetFontsizeFrominiFile()
{
	int notesFontsize = GetPrivateProfileInt(NOTES_INI_SEC, "Fontsize", -666, g_SNM_IniFn.Get());
	if (notesFontsize != -666)
	{
		// first try to get current font, if this doesn't work use theme font as fallback
		HFONT hfont = (HFONT)SendMessage(m_edit, WM_GETFONT, 0, 0);
		if (!hfont)
			hfont = SNM_GetThemeFont()->GetHFont();

		LOGFONT lf;
		GetObject(hfont, sizeof(lf), &lf);
		lf.lfHeight = notesFontsize;
		SendMessage(m_edit, WM_SETFONT, (WPARAM)CreateFontIndirect(&lf), TRUE);
	}
}

void NotesWnd::SetWrapText(const bool wrap)
{
	g_wrapText = wrap;

	static char s_wrapBuf[sizeof(g_lastText)];
	GetWindowText(m_edit, s_wrapBuf, sizeof(s_wrapBuf));
	m_edit = GetDlgItem(m_hwnd, wrap ? IDC_EDIT2 : IDC_EDIT1);
	SetFontsizeFrominiFile();
	m_settingText = true;
	SetWindowText(m_edit, s_wrapBuf);
	m_settingText = false;

#ifdef _WIN32
	// avoid flickering on Windows
	SendMessage(m_hwnd, WM_SETREDRAW, 0, 0);
#endif
	ShowWindow(GetDlgItem(m_hwnd, IDC_EDIT1), wrap ? SW_HIDE : SW_SHOW);
	ShowWindow(GetDlgItem(m_hwnd, IDC_EDIT2), wrap ? SW_SHOW : SW_HIDE);
#ifdef _WIN32
	SendMessage(m_hwnd, WM_SETREDRAW, 1, 0);
	InvalidateRect(m_hwnd, nullptr, 0);
#endif
}

static const char *GetLinkedActorForDisplay(const char *actorName) {
  if (!g_displayActorInPrefix || !actorName || !*actorName || !strcmp(actorName, "?"))
    return NULL;
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *a = actors->Get(i);
    if (!strcmp(a->GetName(), actorName) && a->HasLinkedActor())
      return a->GetLinkedActorName();
  }
  return NULL;
}

static void BuildRegionName(WDL_String *out, const char *actor, const char *notes) {
  if (actor && *actor && strcmp(actor, "?")) {
    const char *linked = GetLinkedActorForDisplay(actor);
    if (linked)
      out->SetFormatted(512, "%s (%s) %s", linked, actor, notes);
    else
      out->SetFormatted(512, "(%s) %s", actor, notes);
  } else {
    out->Set(notes);
  }
  char *p = out->Get();
  while (*p) {
    if (*p == '\r' || *p == '\n') *p = ' ';
    p++;
  }
  out->Ellipsize(0, 64);
}

static void AppendDisplayLine(WDL_FastString *out, const char *actorName, const char *notes) {
  if (actorName && *actorName) {
    const char *linked = GetLinkedActorForDisplay(actorName);
    if (linked)
      out->AppendFormatted(4096, "%s (%s) %s", linked, actorName, notes);
    else
      out->AppendFormatted(4096, "(%s) %s", actorName, notes);
  } else {
    out->Append(notes);
  }
}

void DeleteActorRegions(const char *actorName) {
  InternalMkrRgnGuard mkrGuard;
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  for (int i = subs->GetSize() - 1; i >= 0; i--) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (!strcmp(sub->GetActor(), actorName) && sub->IsValid()) {
      double pos, endPos;
      if (EnumMarkerRegionById(NULL, sub->GetId(), NULL, &pos, &endPos, NULL, NULL, NULL) >= 0)
        sub->SetTimes(pos, endPos);
      int idx = GetMarkerRegionIndexFromId(NULL, sub->GetId());
      if (idx >= 0)
        DeleteProjectMarkerByIndex(NULL, idx);
      sub->SetId(-1);
    }
  }
}

void RecreateActorRegions(const char *actorName) {
  if (g_hideRegions) return;
  InternalMkrRgnGuard mkrGuard;
  SNM_Actor *actor = NULL;
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++)
    if (!strcmp(actors->Get(i)->GetName(), actorName)) {
      actor = actors->Get(i);
      break;
    }
  if (!actor) return;

  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  for (int i = 0; i < subs->GetSize(); i++) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (!strcmp(sub->GetActor(), actorName) && !sub->IsValid()) {
      int enumIdx = 0;
      bool isRgn = false;
      double rStart = 0.0, rEnd = 0.0;
      const char *rName = NULL;
      int rNum = 0;
      bool alreadyExists = false;
      while ((enumIdx = EnumProjectMarkers2(NULL, enumIdx, &isRgn, &rStart, &rEnd, &rName, &rNum))) {
        if (isRgn && fabs(rStart - sub->GetStartTime()) < 0.005 && fabs(rEnd - sub->GetEndTime()) < 0.005) {
          int candidateId = MakeMarkerRegionId(rNum, true);
          bool idUsed = false;
          for (int j = 0; j < subs->GetSize(); j++) {
            if (subs->Get(j) != sub && subs->Get(j)->GetId() == candidateId) {
              idUsed = true;
              break;
            }
          }
          if (idUsed)
            continue;

          char actorTag[256];
          snprintf(actorTag, sizeof(actorTag), "(%s)", actorName);
          char actorTagBracket[256];
          snprintf(actorTagBracket, sizeof(actorTagBracket), "[%s]", actorName);
          if (rName && (strstr(rName, actorTag) || strstr(rName, actorTagBracket))) {
            sub->SetId(candidateId);
            alreadyExists = true;
            break;
          }
        }
      }
      if (alreadyExists)
        continue;

      int color = 0;
      if (g_coloredRegions) {
        const char *hl = g_highlightedActor.Get()->Get();
        if (hl && *hl) {
          if (IsActorMatchingHighlight(actor, hl))
            color = actor->GetEffectiveColor();
        } else {
          color = actor->GetEffectiveColor();
        }
      }
      WDL_String regionName;
      BuildRegionName(&regionName, sub->GetActor(), sub->GetNotes());
      int num = AddProjectMarker2(NULL,
                                  true,
                                  sub->GetStartTime(),
                                  sub->GetEndTime(),
                                  regionName.Get(),
                                  -1,
                                  color);
      if (num >= 0) {
        int newId = MakeMarkerRegionId(num, true);
        sub->SetId(newId);
      }
    }
  }
}

void RecreateNoActorRegions() {
  if (g_hideRegions) return;
  InternalMkrRgnGuard mkrGuard;
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  for (int i = 0; i < subs->GetSize(); i++) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    const char *act = sub->GetActor();
    bool isNoActor = (!act || !*act || !strcmp(act, "?"));
    if (isNoActor && !sub->IsValid()) {
      int enumIdx = 0;
      bool isRgn = false;
      double rStart = 0.0, rEnd = 0.0;
      const char *rName = NULL;
      int rNum = 0;
      bool alreadyExists = false;
      while ((enumIdx = EnumProjectMarkers2(NULL, enumIdx, &isRgn, &rStart, &rEnd, &rName, &rNum))) {
        if (isRgn && fabs(rStart - sub->GetStartTime()) < 0.005 && fabs(rEnd - sub->GetEndTime()) < 0.005) {
          int candidateId = MakeMarkerRegionId(rNum, true);
          bool idUsed = false;
          for (int j = 0; j < subs->GetSize(); j++) {
            if (subs->Get(j) != sub && subs->Get(j)->GetId() == candidateId) {
              idUsed = true;
              break;
            }
          }
          if (idUsed)
            continue;
          sub->SetId(candidateId);
          alreadyExists = true;
          break;
        }
      }
      if (alreadyExists)
        continue;

      WDL_String regionName;
      BuildRegionName(&regionName, "", sub->GetNotes());
      int num = AddProjectMarker2(NULL,
                                  true,
                                  sub->GetStartTime(),
                                  sub->GetEndTime(),
                                  regionName.Get(),
                                  -1,
                                  0);
      if (num >= 0) {
        int newId = MakeMarkerRegionId(num, true);
        sub->SetId(newId);
      }
    }
  }
}

static const char *RENOTES_ITEMS_TRACK_TAG = "P_EXT:SWS_RENOTES_SUBTITLE_ITEMS";
static const char *RENOTES_ITEM_KEY_TAG = "P_EXT:SWS_RENOTES_SUBTITLE_KEY";

static MediaTrack *FindSubtitleItemsTrack() {
  char tag[16];
  for (int i = 0; i < GetNumTracks(); i++) {
    MediaTrack *track = GetTrack(NULL, i);
    tag[0] = '\0';
    if (track && GetSetMediaTrackInfo_String(track, RENOTES_ITEMS_TRACK_TAG, tag, false)
        && !strcmp(tag, "1"))
      return track;
  }
  return NULL;
}

static SNM_Actor *FindActor(const char *name) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *actor = actors->Get(i);
    if (actor && !strcmp(actor->GetName(), name))
      return actor;
  }
  return NULL;
}

struct SubtitleItemEntry {
  SNM_RegionSubtitle *subtitle;
  SNM_Actor *actor;
  double start;
  double end;
  int sourceIndex;
  int lane;
  int laneCount;
  MediaItem *item;
};

static void BuildSubtitleItemKey(const SubtitleItemEntry &entry, WDL_FastString *key) {
  const char *act = entry.actor ? entry.actor->GetName() : (entry.subtitle->GetActor() ? entry.subtitle->GetActor() : "");
  key->SetFormatted(512, "%d:%s", entry.sourceIndex, act);
}

static void BuildSubtitleItemText(const SubtitleItemEntry &entry, WDL_FastString *text) {
  const char *act = entry.actor ? entry.actor->GetName() : (entry.subtitle->GetActor() ? entry.subtitle->GetActor() : "");
  if (act && *act && strcmp(act, "?") != 0)
    text->AppendFormatted(512, "[%s] %s", act, entry.subtitle->GetNotes());
  else
    text->Append(entry.subtitle->GetNotes());
}

void SyncSubtitleItemsTrack(bool createIfMissing) {
  MediaTrack *track = FindSubtitleItemsTrack();
  if (!track && !createIfMissing)
    return;

  Undo_BeginBlock2(NULL);
  if (!track) {
    const int index = GetNumTracks();
    InsertTrackAtIndex(index, true);
    track = GetTrack(NULL, index);
    if (!track) {
      Undo_EndBlock2(NULL, __LOCALIZE("ReNotes: создать дорожку реплик", "sws_DLG_152"),
                     UNDO_STATE_TRACKCFG);
      return;
    }
    GetSetMediaTrackInfo_String(track, "P_NAME", (char *) "ReNotes Subtitles", true);
    GetSetMediaTrackInfo_String(track, RENOTES_ITEMS_TRACK_TAG, (char *) "1", true);
  }

  PreventUIRefresh(1);

  std::unordered_map<std::string, MediaItem *> existingItems;
  std::vector<MediaItem *> staleItems;
  char itemKey[1024];
  for (int i = 0; i < CountTrackMediaItems(track); i++) {
    MediaItem *item = GetTrackMediaItem(track, i);
    itemKey[0] = '\0';
    if (item && GetSetMediaItemInfo_String(item, RENOTES_ITEM_KEY_TAG, itemKey, false)
        && itemKey[0] && existingItems.find(itemKey) == existingItems.end())
      existingItems[itemKey] = item;
    else if (item)
      staleItems.push_back(item);
  }

  std::vector<SubtitleItemEntry> entries;
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subtitles = g_pRegionSubs.Get();
  for (int i = 0; i < subtitles->GetSize(); i++) {
    SNM_RegionSubtitle *subtitle = subtitles->Get(i);
    if (!subtitle || !subtitle->GetNotesLength())
      continue;
    SNM_Actor *actor = NULL;
    if (subtitle->GetActor() && subtitle->GetActor()[0] && strcmp(subtitle->GetActor(), "?") != 0) {
      actor = FindActor(subtitle->GetActor());
      if (actor && !actor->IsEnabled())
        continue;
    }

    double start = subtitle->GetStartTime();
    double end = subtitle->GetEndTime();
    if (subtitle->IsValid()
        && EnumMarkerRegionById(NULL, subtitle->GetId(), NULL, &start, &end,
                                NULL, NULL, NULL) < 0)
      continue;
    if (end <= start)
      continue;
    subtitle->SetTimes(start, end);
    entries.push_back({subtitle, actor, start, end, i, 0, 1, NULL});
  }

  std::sort(entries.begin(), entries.end(), [](const SubtitleItemEntry &a,
                                                const SubtitleItemEntry &b) {
    if (a.start != b.start) return a.start < b.start;
    if (a.end != b.end) return a.end < b.end;
    const char *actA = a.subtitle->GetActor() ? a.subtitle->GetActor() : "";
    const char *actB = b.subtitle->GetActor() ? b.subtitle->GetActor() : "";
    return strcmp(actA, actB) < 0;
  });

  for (size_t groupStart = 0; groupStart < entries.size();) {
    size_t groupEnd = groupStart + 1;
    double groupEndTime = entries[groupStart].end;
    while (groupEnd < entries.size()
           && entries[groupEnd].start < groupEndTime - 0.000001) {
      groupEndTime = std::max(groupEndTime, entries[groupEnd].end);
      groupEnd++;
    }

    std::vector<double> laneEnds;
    for (size_t i = groupStart; i < groupEnd; i++) {
      int lane = 0;
      while (lane < (int) laneEnds.size()
             && entries[i].start < laneEnds[lane] - 0.000001)
        lane++;
      if (lane == (int) laneEnds.size())
        laneEnds.push_back(entries[i].end);
      else
        laneEnds[lane] = entries[i].end;
      entries[i].lane = lane;
    }
    const int laneCount = std::max(1, (int) laneEnds.size());
    for (size_t i = groupStart; i < groupEnd; i++)
      entries[i].laneCount = laneCount;
    groupStart = groupEnd;
  }

  SetMediaTrackInfo_Value(track, "I_FREEMODE", 1.0);

  for (SubtitleItemEntry &entry : entries) {
    WDL_FastString key;
    BuildSubtitleItemKey(entry, &key);
    const auto oldItem = existingItems.find(key.Get());
    MediaItem *item = oldItem == existingItems.end()
      ? AddMediaItemToTrack(track) : oldItem->second;
    if (!item)
      continue;
    const bool isNew = oldItem == existingItems.end();
    if (!isNew)
      existingItems.erase(oldItem);
    else
      GetSetMediaItemInfo_String(item, RENOTES_ITEM_KEY_TAG, (char *) key.Get(), true);
    entry.item = item;

    SetMediaItemInfo_Value(item, "D_POSITION", entry.start);
    SetMediaItemInfo_Value(item, "D_LENGTH", entry.end - entry.start);
    int itemColor = 0;
    if (entry.actor) {
      const char *hl = g_highlightedActor.Get()->Get();
      if (hl && *hl) {
        if (IsActorMatchingHighlight(entry.actor, hl))
          itemColor = entry.actor->GetEffectiveColor();
      } else {
        itemColor = entry.actor->GetEffectiveColor();
      }
    }
    SetMediaItemInfo_Value(item, "I_CUSTOMCOLOR", itemColor);
    SetMediaItemInfo_Value(item, "F_FREEMODE_Y",
                           (double) entry.lane / entry.laneCount);
    SetMediaItemInfo_Value(item, "F_FREEMODE_H", 1.0 / entry.laneCount);

    WDL_FastString itemText;
    BuildSubtitleItemText(entry, &itemText);
    const char *oldText = (const char *) GetSetMediaItemInfo(item, "P_NOTES", NULL);
    if (!oldText || strcmp(oldText, itemText.Get()))
      GetSetMediaItemInfo(item, "P_NOTES", (void *) itemText.Get());
    if (isNew)
      BR_SetMediaItemImageResource(item, "", 3 | 8);
  }

  for (const auto &oldItem : existingItems)
    staleItems.push_back(oldItem.second);
  for (MediaItem *item : staleItems)
    DeleteTrackMediaItem(track, item);

  MarkProjectDirty(NULL);
  TrackList_AdjustWindows(false);
  UpdateTimeline();
  SyncSubtitlesToVideoOverlay();
  PreventUIRefresh(-1);
  Undo_EndBlock2(NULL, __LOCALIZE("ReNotes: обновить дорожку реплик", "sws_DLG_152"),
                 UNDO_STATE_ITEMS | UNDO_STATE_TRACKCFG);
}

static const char* const g_reNotesVideoProcessorCode =
"// ReNotes Subtitles Overlay v3.2 — project_time / video track\n"
"//@gmem=subtitle_reader\n"
"//@param 1:PosY 'Y (0=верх, 1=низ)' 0.88 0 1 0.5 0.01\n"
"//@param 2:Scale 'Масштаб' 1.0 0.4 2.5 1.0 0.05\n"
"//@param 3:MaxWidth 'Ширина текста' 0.90 0.50 1.00 0.75 0.01\n"
"//@param 4:BgAlpha 'Фон' 0.55 0 0.85 0.4 0.01\n"
"//@param 5:ColorMode 'Цвет: 0=Белый 1=Роль 2=Всё' 1 0 2 1 1\n"
"//@param 6:TextR 'Текст R' 1 0 1 0.5 0.01\n"
"//@param 7:TextG 'Текст G' 1 0 1 0.5 0.01\n"
"//@param 8:TextB 'Текст B' 1 0 1 0.5 0.01\n"
"\n"
"iw = 0;\n"
"ih = 0;\n"
"src = -1;\n"
"input_info(0, iw, ih) ? src = 0;\n"
"src < 0 ? src = input_track(0);\n"
"src >= 0 ? gfx_blit(src, 1, 0, 0, project_w, project_h);\n"
"\n"
"fmt = gmem[0]|0;\n"
"count = gmem[1]|0;\n"
"text_base = gmem[2]|0;\n"
"gen = gmem[7]|0;\n"
"gen != cache_gen ? (\n"
"  cache_ok = 0;\n"
"  cache_gen = gen;\n"
");\n"
"\n"
"t = project_time;\n"
"gmem[5] > 0.5 ? t = gmem[4];\n"
"t < 0 ? t = 0;\n"
"count > 20000 ? count = 20000;\n"
"\n"
"need = (fmt != 92003) || (count <= 0) || !cache_ok || (t < cache_st) || (t >= cache_en);\n"
"need ? (\n"
"  #substr = \"\";\n"
"  #one = \"\";\n"
"  have = 0;\n"
"  substr_len = 0;\n"
"  cache_st = 0;\n"
"  cache_en = 999999999;\n"
"  cache_ok = 0;\n"
"  fmt == 92003 && count > 0 ? (\n"
"    act_count = 0;\n"
"    i = 0;\n"
"    while (i < count) (\n"
"      base = 16 + i * 5;\n"
"      st = gmem[base];\n"
"      en = gmem[base + 1];\n"
"      st > t ? (\n"
"        st < cache_en ? cache_en = st;\n"
"        i = count;\n"
"      ) : (\n"
"        t < en ? (\n"
"          act_count < 100 ? (\n"
"            40000[act_count] = i;\n"
"            act_count += 1;\n"
"          );\n"
"          st > cache_st ? cache_st = st;\n"
"          en < cache_en ? cache_en = en;\n"
"        ) : (\n"
"          en > cache_st ? cache_st = en;\n"
"        );\n"
"        i += 1;\n"
"      );\n"
"    );\n"
"    p = 0;\n"
"    while (p < act_count - 1) (\n"
"      q = p + 1;\n"
"      while (q < act_count) (\n"
"        idx_p = 40000[p];\n"
"        idx_q = 40000[q];\n"
"        st_p = gmem[16 + idx_p * 5];\n"
"        st_q = gmem[16 + idx_q * 5];\n"
"        st_q > st_p ? (\n"
"          40000[p] = idx_q;\n"
"          40000[q] = idx_p;\n"
"        );\n"
"        q += 1;\n"
"      );\n"
"      p += 1;\n"
"    );\n"
"    k = 0;\n"
"    while (k < act_count) (\n"
"      c_idx = 40000[k];\n"
"      base = 16 + c_idx * 5;\n"
"      off = gmem[base + 2]|0;\n"
"      len = gmem[base + 3]|0;\n"
"      col = gmem[base + 4]|0;\n"
"      len < 0 ? len = 0;\n"
"      len > 4096 ? len = 4096;\n"
"      cue_start = substr_len;\n"
"      have ? (\n"
"        sprintf(#one, \"%c\", 10);\n"
"        strcat(#substr, #one);\n"
"        20000[substr_len] = col;\n"
"        substr_len += 1;\n"
"        cue_start = substr_len;\n"
"      );\n"
"      j = 0;\n"
"      while (j < len) (\n"
"        c_byte = gmem[text_base + off + j]|0;\n"
"        c_byte != 13 ? (\n"
"          sprintf(#one, \"%c\", c_byte);\n"
"          strcat(#substr, #one);\n"
"          20000[substr_len] = col;\n"
"          substr_len += 1;\n"
"        );\n"
"        j += 1;\n"
"      );\n"
"      20000[cue_start] = col | 268435456;\n"
"      have += 1;\n"
"      k += 1;\n"
"    );\n"
"    20000[substr_len] = 0;\n"
"    cache_ok = 1;\n"
"  );\n"
"  layout_needed = 1;\n"
");\n"
"\n"
"(Scale != last_scale) || (MaxWidth != last_maxw) || (project_w != last_pw) || (project_h != last_ph) ? (\n"
"  layout_needed = 1;\n"
"  last_scale = Scale;\n"
"  last_maxw = MaxWidth;\n"
"  last_pw = project_w;\n"
"  last_ph = project_h;\n"
");\n"
"\n"
"layout_needed ? (\n"
"  layout_needed = 0;\n"
"  fit_lines = 0;\n"
"  block_w = 0;\n"
"  #l0 = \"\"; #l1 = \"\"; #l2 = \"\"; #l3 = \"\";\n"
"  #l4 = \"\"; #l5 = \"\"; #l6 = \"\"; #l7 = \"\";\n"
"  have ? (\n"
"    max_w = project_w * MaxWidth;\n"
"    fs = project_h * 0.048 * Scale;\n"
"    fs < 14 ? fs = 14;\n"
"    fs > 140 ? fs = 140;\n"
"    gfx_setfont(fs, \"Arial\", \"S\");\n"
"    #sp = \" \";\n"
"    #line = \"\";\n"
"    #word = \"\";\n"
"    #test = \"\";\n"
"    #one = \"\";\n"
"    line_char_start = 0;\n"
"    word_start = 0;\n"
"    line_has = 0;\n"
"    i = 0;\n"
"    while (i <= substr_len) (\n"
"      c = i < substr_len ? str_getchar(#substr, i) : 32;\n"
"      c == 13 ? c = 32;\n"
"      brk = (i >= substr_len) || (c == 10) || (c == 32);\n"
"      brk ? (\n"
"        strlen(#word) > 0 ? (\n"
"          line_has ? (\n"
"            #test = #line;\n"
"            strcat(#test, #sp);\n"
"            strcat(#test, #word);\n"
"          ) : (\n"
"            #test = #word;\n"
"          );\n"
"          gfx_str_measure(#test, tw, th);\n"
"          tw <= max_w || !line_has ? (\n"
"            #line = #test;\n"
"            line_has = 1;\n"
"            tw > block_w ? block_w = tw;\n"
"          ) : (\n"
"            fit_lines += 1;\n"
"            fit_lines == 1 ? ( #l0 = #line; ) :\n"
"            fit_lines == 2 ? ( #l1 = #line; ) :\n"
"            fit_lines == 3 ? ( #l2 = #line; ) :\n"
"            fit_lines == 4 ? ( #l3 = #line; ) :\n"
"            fit_lines == 5 ? ( #l4 = #line; ) :\n"
"            fit_lines == 6 ? ( #l5 = #line; ) :\n"
"            fit_lines == 7 ? ( #l6 = #line; ) :\n"
"                             ( #l7 = #line; );\n"
"            30000[fit_lines - 1] = 20000[line_char_start];\n"
"            #line = #word;\n"
"            line_has = 1;\n"
"            line_char_start = word_start;\n"
"            gfx_str_measure(#line, tw, th);\n"
"            tw > block_w ? block_w = tw;\n"
"          );\n"
"          #word = \"\";\n"
"        );\n"
"        c == 10 ? (\n"
"          fit_lines += 1;\n"
"          fit_lines == 1 ? ( #l0 = #line; ) :\n"
"          fit_lines == 2 ? ( #l1 = #line; ) :\n"
"          fit_lines == 3 ? ( #l2 = #line; ) :\n"
"          fit_lines == 4 ? ( #l3 = #line; ) :\n"
"          fit_lines == 5 ? ( #l4 = #line; ) :\n"
"          fit_lines == 6 ? ( #l5 = #line; ) :\n"
"          fit_lines == 7 ? ( #l6 = #line; ) :\n"
"                           ( #l7 = #line; );\n"
"          30000[fit_lines - 1] = 20000[line_char_start];\n"
"          #line = \"\";\n"
"          line_has = 0;\n"
"          line_char_start = i + 1;\n"
"        );\n"
"      ) : (\n"
"        strlen(#word) == 0 ? word_start = i;\n"
"        sprintf(#one, \"%c\", c);\n"
"        strcat(#word, #one);\n"
"      );\n"
"      i += 1;\n"
"    );\n"
"    line_has ? (\n"
"      fit_lines += 1;\n"
"      fit_lines == 1 ? ( #l0 = #line; ) :\n"
"      fit_lines == 2 ? ( #l1 = #line; ) :\n"
"      fit_lines == 3 ? ( #l2 = #line; ) :\n"
"      fit_lines == 4 ? ( #l3 = #line; ) :\n"
"      fit_lines == 5 ? ( #l4 = #line; ) :\n"
"      fit_lines == 6 ? ( #l5 = #line; ) :\n"
"      fit_lines == 7 ? ( #l6 = #line; ) :\n"
"                       ( #l7 = #line; );\n"
"      30000[fit_lines - 1] = 20000[line_char_start];\n"
"    );\n"
"    fit_lines > 8 ? fit_lines = 8;\n"
"  );\n"
");\n"
"\n"
"have && fit_lines > 0 ? (\n"
"  fs = project_h * 0.048 * Scale;\n"
"  fs < 14 ? fs = 14;\n"
"  fs > 140 ? fs = 140;\n"
"  gfx_setfont(fs, \"Arial\", \"S\");\n"
"  gfx_str_measure(\"Ag\", dw, line_h);\n"
"\n"
"  total_h = fit_lines * line_h;\n"
"  pad = line_h * 0.18;\n"
"  y = project_h * PosY - total_h;\n"
"  y < pad ? y = pad;\n"
"\n"
"  BgAlpha > 0.01 ? (\n"
"    bx = (project_w - block_w) * 0.5 - pad * 2;\n"
"    bw = block_w + pad * 4;\n"
"    bx < 0 ? bx = 0;\n"
"    bw > project_w ? bw = project_w;\n"
"    gfx_set(0, 0, 0, BgAlpha);\n"
"    gfx_fillrect(bx, y - pad, bw, total_h + pad * 2);\n"
"  );\n"
"\n"
"  line_idx = 0;\n"
"  while (line_idx < fit_lines) (\n"
"    line_idx == 0 ? ( #draw_line = #l0; ) :\n"
"    line_idx == 1 ? ( #draw_line = #l1; ) :\n"
"    line_idx == 2 ? ( #draw_line = #l2; ) :\n"
"    line_idx == 3 ? ( #draw_line = #l3; ) :\n"
"    line_idx == 4 ? ( #draw_line = #l4; ) :\n"
"    line_idx == 5 ? ( #draw_line = #l5; ) :\n"
"    line_idx == 6 ? ( #draw_line = #l6; ) :\n"
"                    ( #draw_line = #l7; );\n"
"\n"
"    gfx_str_measure(#draw_line, lw, lh);\n"
"    lx = (project_w - lw) * 0.5;\n"
"\n"
"    col_val = 30000[line_idx];\n"
"    cur_col = col_val & 16777215;\n"
"\n"
"    r = TextR; g = TextG; b = TextB;\n"
"    cur_col > 0 && ColorMode > 0 ? (\n"
"      is_hdr = (col_val & 268435456) && (str_getchar(#draw_line, 0) == 91);\n"
"      (ColorMode == 2 || is_hdr) ? (\n"
"        r = ((cur_col >> 16) & 255) / 255;\n"
"        g = ((cur_col >> 8) & 255) / 255;\n"
"        b = (cur_col & 255) / 255;\n"
"      );\n"
"    );\n"
"\n"
"    gfx_set(0, 0, 0, 1);\n"
"    gfx_str_draw(#draw_line, lx - 2, y);\n"
"    gfx_str_draw(#draw_line, lx + 2, y);\n"
"    gfx_str_draw(#draw_line, lx, y - 2);\n"
"    gfx_str_draw(#draw_line, lx, y + 2);\n"
"    gfx_set(r, g, b, 1);\n"
"    gfx_str_draw(#draw_line, lx, y);\n"
"    y += line_h;\n"
"    line_idx += 1;\n"
"  );\n"
");\n";

static bool SourceIsVideo(PCM_source *src) {
  if (!src) return false;
  char typ[128] = "";
  GetMediaSourceType(src, typ, sizeof(typ));
  if (!_stricmp(typ, "VIDEO") || strstr(typ, "VIDEO"))
    return true;
  char fn[2048] = "";
  GetMediaSourceFileName(src, fn, sizeof(fn));
  const char *ext = strrchr(fn, '.');
  if (ext) {
    ext++;
    static const char *const s_videoExts[] = {
      "mp4", "mkv", "avi", "mov", "webm", "m4v", "wmv",
      "ts", "m2ts", "mpg", "mpeg", "m2v", "flv", "ogv", "vob"
    };
    for (size_t i = 0; i < sizeof(s_videoExts)/sizeof(s_videoExts[0]); i++) {
      if (!_stricmp(ext, s_videoExts[i]))
        return true;
    }
  }
  return false;
}

static bool TrackHasVideo(MediaTrack *tr) {
  if (!tr) return false;
  const int count = GetTrackNumMediaItems(tr);
  for (int i = 0; i < count; i++) {
    MediaItem *item = GetTrackMediaItem(tr, i);
    MediaItem_Take *take = GetActiveTake(item);
    if (take && !TakeIsMIDI(take)) {
      if (SourceIsVideo(GetMediaItemTake_Source(take)))
        return true;
    }
  }
  return false;
}

static int FindVideoSubtitleFX(MediaTrack *tr) {
  if (!tr) return -1;
  const int count = TrackFX_GetCount(tr);
  for (int i = 0; i < count; i++) {
    char renamed[256] = "";
    if (TrackFX_GetNamedConfigParm && TrackFX_GetNamedConfigParm(tr, i, "renamed_name", renamed, sizeof(renamed))) {
      if (!strcmp(renamed, "ReNotes Subtitles") || !strcmp(renamed, "Subtitle Reader"))
        return i;
    }
    char name[256] = "";
    TrackFX_GetFXName(tr, i, name, sizeof(name));
    if (strstr(name, "ReNotes Subtitles") || strstr(name, "Subtitle Reader"))
      return i;
    char codeBuf[512] = "";
    if (TrackFX_GetNamedConfigParm && TrackFX_GetNamedConfigParm(tr, i, "VIDEO_CODE", codeBuf, sizeof(codeBuf))) {
      if (strstr(codeBuf, "subtitle_reader") || strstr(codeBuf, "ReNotes Subtitles"))
        return i;
    }
  }
  return -1;
}

static MediaTrack *FindVideoTrack(bool createIfMissing = false) {
  const int count = GetNumTracks();

  // 1. Check if any track already has ReNotes subtitle FX
  for (int i = 0; i < count; i++) {
    MediaTrack *tr = GetTrack(NULL, i);
    if (!tr) continue;
    char tag[16] = "";
    GetSetMediaTrackInfo_String(tr, RENOTES_ITEMS_TRACK_TAG, tag, false);
    if (tag[0]) continue;
    if (FindVideoSubtitleFX(tr) >= 0)
      return tr;
  }

  // 2. Check for tracks with actual video media
  for (int i = 0; i < count; i++) {
    MediaTrack *tr = GetTrack(NULL, i);
    if (!tr) continue;
    char tag[16] = "";
    GetSetMediaTrackInfo_String(tr, RENOTES_ITEMS_TRACK_TAG, tag, false);
    if (tag[0]) continue;
    char name[256] = "";
    GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
    if (!_stricmp(name, "Subtitle Overlay (Auto)")) continue;
    if (TrackHasVideo(tr))
      return tr;
  }

  // 3. Check for tracks with video-related names
  for (int i = 0; i < count; i++) {
    MediaTrack *tr = GetTrack(NULL, i);
    if (!tr) continue;
    char tag[16] = "";
    GetSetMediaTrackInfo_String(tr, RENOTES_ITEMS_TRACK_TAG, tag, false);
    if (tag[0]) continue;
    char name[256] = "";
    GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
    if (strstr(name, "Video") || strstr(name, "video") ||
        strstr(name, "Видео") || strstr(name, "видео") ||
        strstr(name, "Movie") || strstr(name, "movie") ||
        strstr(name, "Film") || strstr(name, "film") ||
        strstr(name, "Фильм") || strstr(name, "фильм") ||
        strstr(name, "Серия") || strstr(name, "серия")) {
      return tr;
    }
  }

  // 4. Selected track (if not subtitle items track)
  MediaTrack *sel = GetSelectedTrack(NULL, 0);
  if (sel) {
    char tag[16] = "";
    GetSetMediaTrackInfo_String(sel, RENOTES_ITEMS_TRACK_TAG, tag, false);
    if (!tag[0])
      return sel;
  }

  // 5. First non-subtitle track in project
  for (int i = 0; i < count; i++) {
    MediaTrack *tr = GetTrack(NULL, i);
    if (!tr) continue;
    char tag[16] = "";
    GetSetMediaTrackInfo_String(tr, RENOTES_ITEMS_TRACK_TAG, tag, false);
    if (tag[0]) continue;
    return tr;
  }

  // 6. Create Video track if requested
  if (createIfMissing) {
    InsertTrackAtIndex(0, true);
    MediaTrack *newTr = GetTrack(NULL, 0);
    if (newTr) {
      GetSetMediaTrackInfo_String(newTr, "P_NAME", (char *)"Video", true);
      TrackList_AdjustWindows(false);
      UpdateTimeline();
      return newTr;
    }
  }

  return NULL;
}

static void SetTrackFXParamIfChanged(MediaTrack *tr, int fx, int idx, double val) {
  if (TrackFX_GetParam) {
    double minV = 0.0, maxV = 0.0;
    double curVal = TrackFX_GetParam(tr, fx, idx, &minV, &maxV);
    if (fabs(curVal - val) < 0.0001)
      return;
  }
  TrackFX_SetParam(tr, fx, idx, val);
}

static void ApplyVideoSubtitleFXParams(MediaTrack *tr, int fx) {
  if (!tr || fx < 0) return;
  SetTrackFXParamIfChanged(tr, fx, 0, g_videoOverlayPosY);
  SetTrackFXParamIfChanged(tr, fx, 1, g_videoOverlayScale);
  SetTrackFXParamIfChanged(tr, fx, 2, g_videoOverlayMaxWidth);
  SetTrackFXParamIfChanged(tr, fx, 3, g_videoOverlayBgAlpha);
  SetTrackFXParamIfChanged(tr, fx, 4, (double)g_videoOverlayColorMode);
  SetTrackFXParamIfChanged(tr, fx, 5, 1.0);
  SetTrackFXParamIfChanged(tr, fx, 6, 1.0);
  SetTrackFXParamIfChanged(tr, fx, 7, 1.0);
}

static int EnsureVideoSubtitleFX(MediaTrack *tr, bool enable = true) {
  if (!tr) return -1;
  int fx = FindVideoSubtitleFX(tr);
  if (fx < 0) {
    if (TrackFX_AddByName)
      fx = TrackFX_AddByName(tr, "Video processor", false, -1);
    if (fx >= 0 && TrackFX_SetNamedConfigParm) {
      TrackFX_SetNamedConfigParm(tr, fx, "VIDEO_CODE", g_reNotesVideoProcessorCode);
      TrackFX_SetNamedConfigParm(tr, fx, "renamed_name", "ReNotes Subtitles");
      TrackFX_SetOpen(tr, fx, false);
    }
  } else {
    if (TrackFX_SetNamedConfigParm) {
      char codeBuf[256] = "";
      if (!TrackFX_GetNamedConfigParm || !TrackFX_GetNamedConfigParm(tr, fx, "VIDEO_CODE", codeBuf, sizeof(codeBuf)) ||
          !strstr(codeBuf, "v3.2")) {
        TrackFX_SetNamedConfigParm(tr, fx, "VIDEO_CODE", g_reNotesVideoProcessorCode);
        TrackFX_SetNamedConfigParm(tr, fx, "renamed_name", "ReNotes Subtitles");
      }
    }
  }
  if (fx >= 0) {
    bool isEnabled = TrackFX_GetEnabled ? TrackFX_GetEnabled(tr, fx) : false;
    if (isEnabled != enable) {
      TrackFX_SetEnabled(tr, fx, enable);
    }
    ApplyVideoSubtitleFXParams(tr, fx);
  }
  return fx;
}

static void MigrateOldOverlayFX() {
  const int count = GetNumTracks();
  for (int i = 0; i < count; i++) {
    MediaTrack *tr = GetTrack(NULL, i);
    if (!tr) continue;
    char name[256] = "";
    GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
    if (!_stricmp(name, "Subtitle Overlay (Auto)")) {
      int fx = FindVideoSubtitleFX(tr);
      if (fx >= 0) TrackFX_Delete(tr, fx);
    }
  }
}

#pragma pack(push, 1)
struct ReNotesCueHeader {
  int32_t fmt;
  int32_t count;
  int32_t textBytes;
  int32_t gen;
};

struct ReNotesCueEntry {
  double startTime;
  double endTime;
  int32_t offset;
  int32_t length;
  int32_t color;
};
#pragma pack(pop)

static const char *s_videoSyncLuaCode =
"-- ReNotes Video Overlay gmem sync helper\n"
"local res_path = reaper.GetResourcePath()\n"
"local fn = res_path .. \"/renotes_cues.dat\"\n"
"local f = io.open(fn, \"rb\")\n"
"reaper.gmem_attach(\"subtitle_reader\")\n"
"if not f then\n"
"  reaper.gmem_write(0, 0)\n"
"  reaper.gmem_write(1, 0)\n"
"  reaper.gmem_write(7, (reaper.gmem_read(7) or 0) + 1)\n"
"  reaper.defer(function() end)\n"
"  return\n"
"end\n"
"local data = f:read(\"*a\")\n"
"f:close()\n"
"\n"
"if not data or #data < 16 then\n"
"  reaper.gmem_write(0, 0)\n"
"  reaper.gmem_write(1, 0)\n"
"  reaper.gmem_write(7, (reaper.gmem_read(7) or 0) + 1)\n"
"  reaper.defer(function() end)\n"
"  return\n"
"end\n"
"\n"
"local fmt, count, text_bytes, gen = string.unpack(\"<i4<i4<i4<i4\", data, 1)\n"
"if fmt ~= 92003 or count <= 0 then\n"
"  reaper.gmem_write(0, 0)\n"
"  reaper.gmem_write(1, 0)\n"
"  reaper.gmem_write(7, gen)\n"
"  reaper.defer(function() end)\n"
"  return\n"
"end\n"
"\n"
"local text_base = 16 + count * 5\n"
"reaper.gmem_write(0, 0)\n"
"reaper.gmem_write(1, count)\n"
"reaper.gmem_write(2, text_base)\n"
"reaper.gmem_write(3, text_bytes)\n"
"reaper.gmem_write(4, 0)\n"
"reaper.gmem_write(5, 0)\n"
"\n"
"local ptr = 17\n"
"for i = 0, count - 1 do\n"
"  local st, en, off, len, col = string.unpack(\"<d<d<i4<i4<i4\", data, ptr)\n"
"  ptr = ptr + 28\n"
"  local base = 16 + i * 5\n"
"  reaper.gmem_write(base, st)\n"
"  reaper.gmem_write(base + 1, en)\n"
"  reaper.gmem_write(base + 2, off)\n"
"  reaper.gmem_write(base + 3, len)\n"
"  reaper.gmem_write(base + 4, col)\n"
"end\n"
"\n"
"local pos = text_base\n"
"for i = 1, text_bytes do\n"
"  reaper.gmem_write(pos, data:byte(ptr))\n"
"  pos = pos + 1\n"
"  ptr = ptr + 1\n"
"end\n"
"reaper.gmem_write(0, fmt)\n"
"reaper.gmem_write(7, gen)\n"
"reaper.defer(function() end)\n";

static int s_videoSyncCmdId = 0;

static void EnsureVideoSyncScript() {
  char luaPath[SNM_MAX_PATH] = "";
  snprintf(luaPath, sizeof(luaPath), "%s/ReNotes_VideoSync.lua", GetResourcePath());

  FILE *f = fopen(luaPath, "wb");
  if (f) {
    fputs(s_videoSyncLuaCode, f);
    fclose(f);
  }

  if (s_videoSyncCmdId == 0 && AddRemoveReaScript) {
    s_videoSyncCmdId = AddRemoveReaScript(true, 0, luaPath, true);
  }
}

static void RunVideoSyncScript() {
  if (GetCurrentProjectInLoadSave && GetCurrentProjectInLoadSave() != NULL)
    return;
  EnsureVideoSyncScript();
  if (s_videoSyncCmdId > 0 && Main_OnCommand) {
    Main_OnCommand(s_videoSyncCmdId, 0);
  }
}

static void ClearSubtitleGmem() {
  char dataPath[SNM_MAX_PATH] = "";
  snprintf(dataPath, sizeof(dataPath), "%s/renotes_cues.dat", GetResourcePath());
  FILE *f = fopen(dataPath, "wb");
  if (f) {
    ReNotesCueHeader hdr = { 0, 0, 0, ++s_gmemGeneration };
    fwrite(&hdr, sizeof(hdr), 1, f);
    fclose(f);
  }
  RunVideoSyncScript();
}

struct VideoCueData {
  double start;
  double end;
  std::string text;
  int off;
  int len;
  int color;
};

void SyncTimelineRegionsToSubtitles() {
  if (g_hideRegions)
    return;
  if (GetCurrentProjectInLoadSave && GetCurrentProjectInLoadSave() != NULL)
    return;

  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  if (!subs)
    return;

  int enumIdx = 0;
  bool isRgn = false;
  double rStart = 0.0, rEnd = 0.0;
  const char *rName = NULL;
  int rNum = 0;

  std::vector<int> timelineRegionIds;

  while ((enumIdx = EnumProjectMarkers2(NULL, enumIdx, &isRgn, &rStart, &rEnd, &rName, &rNum))) {
    if (!isRgn)
      continue;
    int regId = MakeMarkerRegionId(rNum, true);
    timelineRegionIds.push_back(regId);

    SNM_RegionSubtitle *sub = NULL;
    for (int i = 0; i < subs->GetSize(); i++) {
      if (subs->Get(i)->GetId() == regId) {
        sub = subs->Get(i);
        break;
      }
    }

    if (sub) {
      sub->SetTimes(rStart, rEnd);
      if ((!sub->GetNotes() || !*sub->GetNotes()) && rName && *rName) {
        sub->SetNotes(rName);
      }
    } else {
      std::string role;
      std::string notes;
      const char *srcName = rName ? rName : "";
      if (srcName[0] == '[' || srcName[0] == '(') {
        char closeChar = (srcName[0] == '[') ? ']' : ')';
        const char *closePtr = strchr(srcName, closeChar);
        if (closePtr) {
          role.assign(srcName + 1, closePtr - (srcName + 1));
          const char *rest = closePtr + 1;
          while (*rest == ' ' || *rest == '\t')
            rest++;
          notes.assign(rest);
        } else {
          notes.assign(srcName);
        }
      } else {
        notes.assign(srcName);
      }

      SNM_RegionSubtitle *newSub = new SNM_RegionSubtitle(nullptr, regId, notes.c_str());
      if (!role.empty() && role != "?") {
        newSub->SetActor(role.c_str());
        FindOrCreateActor(role.c_str());
      } else {
        newSub->SetActor("");
      }
      newSub->SetTimes(rStart, rEnd);
      subs->Add(newSub);
    }
  }

  for (int i = subs->GetSize() - 1; i >= 0; i--) {
    SNM_RegionSubtitle *s = subs->Get(i);
    if (!s || !s->IsValid())
      continue;
    int sId = s->GetId();
    bool foundOnTimeline = false;
    for (size_t tIdx = 0; tIdx < timelineRegionIds.size(); tIdx++) {
      if (timelineRegionIds[tIdx] == sId) {
        foundOnTimeline = true;
        break;
      }
    }
    if (!foundOnTimeline) {
      subs->Delete(i, true);
    }
  }
}

void SyncSubtitlesToVideoOverlay() {
  if (GetCurrentProjectInLoadSave && GetCurrentProjectInLoadSave() != NULL)
    return;

  static bool s_inSync = false;
  if (s_inSync)
    return;
  struct SyncGuard {
    bool &m_ref;
    SyncGuard(bool &ref) : m_ref(ref) { m_ref = true; }
    ~SyncGuard() { m_ref = false; }
  } guard(s_inSync);

  SyncTimelineRegionsToSubtitles();

  ReNotes_LogMsg("SyncSubtitlesToVideoOverlay: subs=%d", g_pRegionSubs.Get()->GetSize());

  MigrateOldOverlayFX();

  MediaTrack *vtr = FindVideoTrack(false);
  int foundFx = vtr ? FindVideoSubtitleFX(vtr) : -1;

  // If track has ReNotes subtitle FX, check if user enabled it directly in REAPER FX chain
  if (vtr) {
    int fx = foundFx;
    if (fx >= 0 && TrackFX_GetEnabled) {
      if (TrackFX_GetEnabled(vtr, fx) && !g_videoOverlayEnabled) {
        g_videoOverlayEnabled = true;
        WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlay", "1", g_SNM_IniFn.Get());
      }
    }
  }

  if (!g_videoOverlayEnabled) {
    ClearSubtitleGmem();
    return;
  }

  if (vtr) {
    int fx = EnsureVideoSubtitleFX(vtr, true);
    if (fx >= 0)
      ApplyVideoSubtitleFXParams(vtr, fx);
  }

  std::vector<VideoCueData> cues;
  int textBytes = 0;
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subtitles = g_pRegionSubs.Get();

  for (int i = 0; i < subtitles->GetSize(); i++) {
    SNM_RegionSubtitle *sub = subtitles->Get(i);
    if (!sub || !sub->GetNotesLength())
      continue;

    double st = sub->GetStartTime();
    double en = sub->GetEndTime();
    int rgnColor = 0;

    if (!g_hideRegions) {
      if (EnumMarkerRegionById(NULL, sub->GetId(), NULL, &st, &en, NULL, NULL, &rgnColor) >= 0) {
        sub->SetTimes(st, en);
      }
    }
    if (en <= st)
      continue;

    const char *actorName = sub->GetActor();
    SNM_Actor *actor = NULL;
    if (actorName && *actorName && strcmp(actorName, "?") != 0) {
      actor = FindActor(actorName);
    }

    int cueColor = 0;
    int rawCol = (rgnColor & 0x1000000) ? rgnColor : (actor ? actor->GetEffectiveColor() : 0);
    if (rawCol & 0x1000000) {
      int r = rawCol & 0xFF;
      int g = (rawCol >> 8) & 0xFF;
      int b = (rawCol >> 16) & 0xFF;
      double lum = 0.299 * r + 0.587 * g + 0.114 * b;
      if (lum < 110.0 && (r > 0 || g > 0 || b > 0)) {
        double factor = 110.0 / (lum < 1.0 ? 1.0 : lum);
        r = (int)(r * factor);
        g = (int)(g * factor);
        b = (int)(b * factor);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
      }
      cueColor = (r << 16) | (g << 8) | b;
    }

    std::string text;
    if (g_videoOverlayIncludeActor && actorName && *actorName && strcmp(actorName, "?") != 0) {
      text = std::string("[") + actorName + "]\n" + (sub->GetNotes() ? sub->GetNotes() : "");
    } else {
      text = sub->GetNotes() ? sub->GetNotes() : "";
    }

    std::string cleanText;
    cleanText.reserve(text.length());
    for (size_t cIdx = 0; cIdx < text.length(); cIdx++) {
      char c = text[cIdx];
      if (c == '\r') {
        if (cIdx + 1 < text.length() && text[cIdx + 1] == '\n')
          continue;
        c = '\n';
      }
      cleanText.push_back(c);
    }
    while (!cleanText.empty() && (cleanText.back() == '\n' || cleanText.back() == '\r' || cleanText.back() == ' ' || cleanText.back() == '\t')) {
      cleanText.pop_back();
    }
    if (cleanText.empty())
      continue;

    VideoCueData cue;
    cue.start = st;
    cue.end = en;
    cue.text = cleanText;
    cue.off = 0;
    cue.len = static_cast<int>(cleanText.length());
    cue.color = cueColor;
    cues.push_back(cue);
  }

  std::sort(cues.begin(), cues.end(), [](const VideoCueData &a, const VideoCueData &b) {
    if (fabs(a.start - b.start) > 0.0001)
      return a.start < b.start;
    return a.end < b.end;
  });

  const int GMEM_HDR = 16;
  const int GMEM_STRIDE = 5;
  const int GMEM_MAX_CUES = 20000;
  const int GMEM_MAX_SLOTS = 1000000;

  int count = static_cast<int>(cues.size());
  if (count > GMEM_MAX_CUES)
    count = GMEM_MAX_CUES;

  for (int i = 0; i < count; i++) {
    cues[i].off = textBytes;
    textBytes += cues[i].len;
  }
  while (count > 0) {
    int textBase = GMEM_HDR + count * GMEM_STRIDE;
    if (textBase + textBytes < GMEM_MAX_SLOTS)
      break;
    textBytes -= cues[count - 1].len;
    count--;
  }

  static bool s_wasEmpty = false;
  if (count == 0) {
    if (!s_wasEmpty) {
      ClearSubtitleGmem();
      s_wasEmpty = true;
    }
    return;
  }
  s_wasEmpty = false;

  uint32_t hash = 2166136261u;
  auto hashCombine = [&hash](const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; i++) {
      hash ^= p[i];
      hash *= 16777619u;
    }
  };

  hashCombine(&vtr, sizeof(MediaTrack *));
  hashCombine(&g_videoOverlayEnabled, sizeof(bool));
  hashCombine(&g_videoOverlayIncludeActor, sizeof(bool));
  hashCombine(&g_videoOverlayColorMode, sizeof(int));
  hashCombine(&g_videoOverlayPosY, sizeof(double));
  hashCombine(&g_videoOverlayScale, sizeof(double));
  hashCombine(&g_videoOverlayMaxWidth, sizeof(double));
  hashCombine(&g_videoOverlayBgAlpha, sizeof(double));

  for (int i = 0; i < count; i++) {
    hashCombine(&cues[i].start, sizeof(double));
    hashCombine(&cues[i].end, sizeof(double));
    hashCombine(&cues[i].color, sizeof(int));
    hashCombine(cues[i].text.data(), cues[i].len);
  }

  static uint32_t s_lastCuesHash = 0;
  static MediaTrack *s_lastSyncedVtr = NULL;
  static bool s_hasSyncedOnce = false;

  if (s_hasSyncedOnce && hash == s_lastCuesHash && vtr == s_lastSyncedVtr) {
    return;
  }
  s_hasSyncedOnce = true;
  s_lastCuesHash = hash;
  s_lastSyncedVtr = vtr;

  s_gmemGeneration++;

  char dataPath[SNM_MAX_PATH] = "";
  snprintf(dataPath, sizeof(dataPath), "%s/renotes_cues.dat", GetResourcePath());
  FILE *f = fopen(dataPath, "wb");
  if (f) {
    ReNotesCueHeader hdr = { 92003, count, textBytes, s_gmemGeneration };
    fwrite(&hdr, sizeof(hdr), 1, f);
    for (int i = 0; i < count; i++) {
      ReNotesCueEntry entry = { cues[i].start, cues[i].end, cues[i].off, cues[i].len, cues[i].color };
      fwrite(&entry, sizeof(entry), 1, f);
    }
    for (int i = 0; i < count; i++) {
      fwrite(cues[i].text.data(), 1, cues[i].len, f);
    }
    fclose(f);
  }

  RunVideoSyncScript();
}

void ToggleVideoOverlayAction(COMMAND_T *) {
  g_videoOverlayEnabled = !g_videoOverlayEnabled;
  WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlay", g_videoOverlayEnabled ? "1" : "0", g_SNM_IniFn.Get());
  MediaTrack *vtr = FindVideoTrack(true);
  if (vtr) {
    int fx = EnsureVideoSubtitleFX(vtr, g_videoOverlayEnabled);
    if (g_videoOverlayEnabled) {
      if (fx >= 0) ApplyVideoSubtitleFXParams(vtr, fx);
      Help_Set(__LOCALIZE("ReNotes: Субтитры на видео включены", "sws_DLG_152"), false);
    } else {
      if (fx >= 0) TrackFX_SetEnabled(vtr, fx, false);
      Help_Set(__LOCALIZE("ReNotes: Субтитры на видео выключены", "sws_DLG_152"), false);
    }
  }
  SyncSubtitlesToVideoOverlay();
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->RefreshGUI();
  MarkProjectDirty(NULL);
}

int IsVideoOverlayAction(COMMAND_T *) {
  return g_videoOverlayEnabled ? 1 : 0;
}

void RefreshVideoOverlayAction(COMMAND_T *) {
  MediaTrack *vtr = FindVideoTrack(true);
  if (vtr && g_videoOverlayEnabled) {
    int fx = EnsureVideoSubtitleFX(vtr, true);
    if (fx >= 0)
      ApplyVideoSubtitleFXParams(vtr, fx);
  }
  SyncSubtitlesToVideoOverlay();
  Help_Set(__LOCALIZE("ReNotes: Субтитры на видео обновлены", "sws_DLG_152"), false);
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->RefreshGUI();
  MarkProjectDirty(NULL);
}

void UpdateRegionColors() {
  if (g_hideRegions) return;
  InternalMkrRgnGuard mkrGuard;
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();

  PreventUIRefresh(1);

  for (int i = 0; i < subs->GetSize(); i++) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (!sub || !sub->IsValid())
      continue;
    const char *act = sub->GetActor();
    if (!act || !*act || !strcmp(act, "?") || !FindActor(act))
      continue;

    int color = 0;
    if (g_coloredRegions) {
      for (int j = 0; j < actors->GetSize(); j++) {
        if (!strcmp(actors->Get(j)->GetName(), sub->GetActor())) {
          SNM_Actor *subActor = actors->Get(j);
          const char *hl = g_highlightedActor.Get()->Get();
          if (hl && *hl) {
            if (IsActorMatchingHighlight(subActor, hl))
              color = subActor->GetEffectiveColor();
          } else {
            color = subActor->GetEffectiveColor();
          }
          break;
        }
      }
    }

    double pos, endPos;
    int num;
    if (EnumMarkerRegionById(NULL, sub->GetId(), NULL, &pos, &endPos, NULL, &num, NULL) >= 0) {
      WDL_String regionName;
      BuildRegionName(&regionName, sub->GetActor(), sub->GetNotes());
      SetProjectMarker4(NULL, num, true, pos, endPos, regionName.Get(), color, 0);
    }
  }

  PreventUIRefresh(-1);
  UpdateTimeline();
}

static void FormatSubTime(double seconds, char *buf, int bufSize, int formatOverride = -1) {
  int mode = (formatOverride >= 0) ? formatOverride : g_copyTimeFormat;

  if (mode == SNM_TIME_FORMAT_MS) {
    int total_ms = (int)floor(seconds * 1000.0 + 0.5);
    int ms = total_ms % 1000;
    int total_s = total_ms / 1000;
    int s = total_s % 60;
    int total_m = total_s / 60;
    int m = total_m % 60;
    int h = total_m / 60;
    if (h > 0)
      snprintf(buf, bufSize, "%d:%02d:%02d.%03d", h, m, s, ms);
    else
      snprintf(buf, bufSize, "%d:%02d.%03d", m, s, ms);
    return;
  }

  char raw[64] = "";
  format_timestr_pos(seconds, raw, sizeof(raw), 5);

  int tokens[8] = {0};
  int tokenCount = 0;
  const char *p = raw;
  while (*p && tokenCount < 8) {
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (*p && isdigit((unsigned char)*p)) {
      char *next = NULL;
      tokens[tokenCount++] = (int)strtol(p, &next, 10);
      p = next;
    }
  }

  int h = 0, m = 0, s = 0, f = 0;
  if (tokenCount >= 4) {
    h = tokens[0];
    m = tokens[1];
    s = tokens[2];
    f = tokens[3];
  } else if (tokenCount == 3) {
    if (seconds >= 3600.0) {
      h = tokens[0];
      m = tokens[1];
      s = tokens[2];
      f = 0;
    } else {
      h = 0;
      m = tokens[0];
      s = tokens[1];
      f = tokens[2];
    }
  } else if (tokenCount == 2) {
    h = 0;
    m = tokens[0];
    s = tokens[1];
    f = 0;
  } else {
    lstrcpyn(buf, raw, bufSize);
    return;
  }

  if (mode == SNM_TIME_FORMAT_FRAMES) {
    if (h > 0)
      snprintf(buf, bufSize, "%d:%02d:%02d(%02d)", h, m, s, f);
    else
      snprintf(buf, bufSize, "%d:%02d(%02d)", m, s, f);
  } else {
    // Default: SNM_TIME_FORMAT_NO_FRAMES (0)
    if (h > 0)
      snprintf(buf, bufSize, "%d:%02d:%02d", h, m, s);
    else
      snprintf(buf, bufSize, "%d:%02d", m, s);
  }
}


struct MarkerEntry {
  double pos;
  WDL_FastString text;
};

static void CopyMarkersToClipboardLegacy(HWND hwnd) {
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  if (!subs->GetSize()) return;

  int enumIdx = 0;
  bool isRgn;
  double mPos;
  WDL_TypedBuf<double> markerPositions;
  while ((enumIdx = EnumProjectMarkers2(NULL, enumIdx, &isRgn, &mPos, NULL, NULL, NULL))) {
    if (!isRgn)
      markerPositions.Add(mPos);
  }
  struct SubEntry {
    double startTime;
    double endTime;
    const char *text;
    const char *linkedActorName;
  };

  WDL_PtrList_DOD<WDL_PtrList_DOD<SubEntry> > groups;
  WDL_PtrList_DOD<WDL_FastString> groupNames;

  WDL_PtrList_DOD<SubEntry> unlinkedGroup;
  WDL_FastString unlinkedName("");

  for (int i = 0; i < subs->GetSize(); i++) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (!sub->IsValid()) continue;

    double pos, endPos;
    if (EnumMarkerRegionById(NULL, sub->GetId(), NULL, &pos, &endPos, NULL, NULL, NULL) < 0)
      continue;

    bool hasMarker = false;
    for (int m = 0; m < markerPositions.GetSize(); m++) {
      double mp = markerPositions.Get()[m];
      if (mp >= pos && mp < endPos) {
        hasMarker = true;
        break;
      }
    }
    if (!hasMarker) continue;

    const char *linkedName = "";
    const char *actorName = sub->GetActor();
    if (actorName[0]) {
      for (int j = 0; j < actors->GetSize(); j++) {
        SNM_Actor *a = actors->Get(j);
        if (!strcmp(a->GetName(), actorName)) {
          if (a->HasLinkedActor())
            linkedName = a->GetLinkedActorName();
          break;
        }
      }
    }

    SubEntry *entry = new SubEntry();
    entry->startTime = pos;
    entry->endTime = endPos;
    entry->text = sub->GetNotes();
    entry->linkedActorName = linkedName;

    if (!linkedName[0]) {
      unlinkedGroup.Add(entry);
    } else {
      WDL_PtrList_DOD<SubEntry> *targetGroup = NULL;
      for (int g = 0; g < groupNames.GetSize(); g++) {
        if (!strcmp(groupNames.Get(g)->Get(), linkedName)) {
          targetGroup = groups.Get(g);
          break;
        }
      }
      if (!targetGroup) {
        targetGroup = new WDL_PtrList_DOD<SubEntry>();
        groups.Add(targetGroup);
        groupNames.Add(new WDL_FastString(linkedName));
      }
      targetGroup->Add(entry);
    }
  }

  WDL_FastString output;

  auto appendGroup = [&output](WDL_PtrList_DOD<SubEntry> *grp) {
    for (int a = 0; a < grp->GetSize() - 1; a++) {
      for (int b = a + 1; b < grp->GetSize(); b++) {
        if (grp->Get(a)->startTime > grp->Get(b)->startTime) {
          SubEntry *tmp = grp->Get(a);
          grp->Set(a, grp->Get(b));
          grp->Set(b, tmp);
        }
      }
    }
    for (int j = 0; j < grp->GetSize(); j++) {
      SubEntry *e = grp->Get(j);
      char startBuf[64], endBuf[64];
      FormatSubTime(e->startTime, startBuf, sizeof(startBuf));
      FormatSubTime(e->endTime, endBuf, sizeof(endBuf));

      WDL_FastString text(e->text);
      for (int p = 0; p < text.GetLength();) {
        char c = text.Get()[p];
        bool isBreak = false;
        int breakLen = 0;
        if (c == '\\' && p + 1 < text.GetLength() && (text.Get()[p + 1] == 'N' || text.Get()[p + 1]
          == 'n')) {
          isBreak = true;
          breakLen = 2;
        } else if (c == '\r' && p + 1 < text.GetLength() && text.Get()[p + 1] == '\n') {
          isBreak = true;
          breakLen = 2;
        } else if (c == '\n' || c == '\r') {
          isBreak = true;
          breakLen = 1;
        }
        if (isBreak) {
          int start = p;
          while (start > 0 && text.Get()[start - 1] == ' ') start--;
          int end = p + breakLen;
          while (end < text.GetLength() && text.Get()[end] == ' ') end++;
          text.DeleteSub(start, end - start);
          text.Insert(" ", start);
          p = start + 1;
        } else {
          p++;
        }
      }

      output.AppendFormatted(512, "%s-%s %s\n", startBuf, endBuf, text.Get());
    }
  };

  if (unlinkedGroup.GetSize() > 0) {
    appendGroup(&unlinkedGroup);
  }

  for (int g = 0; g < groups.GetSize(); g++) {
    if (output.GetLength() > 0)
      output.Append("\n");
    output.AppendFormatted(256, "%s\n", groupNames.Get(g)->Get());
    appendGroup(groups.Get(g));
  }

  if (output.GetLength() > 0 && output.Get()[output.GetLength() - 1] == '\n')
    output.DeleteSub(output.GetLength() - 1, 1);

  if (output.GetLength() > 0)
    CF_SetClipboard(output.Get());
  else
    MessageBox(hwnd,
               __LOCALIZE("Нет маркеров внутри регионов.", "sws_DLG_152"),
               __LOCALIZE("ReNotes", "sws_DLG_152"),
               MB_OK);

  for (int i = 0; i < unlinkedGroup.GetSize(); i++)
    delete unlinkedGroup.Get(i);
  for (int g = 0; g < groups.GetSize(); g++) {
    for (int i = 0; i < groups.Get(g)->GetSize(); i++)
      delete groups.Get(g)->Get(i);
  }
}

struct ClipboardMarkerEntry {
  double position;
  std::string text;
};

struct ClipboardRegionEntry {
  double start;
  double end;
  int number;
  std::string text;
};

struct ClipboardTextEntry {
  double start;
  double end;
  std::string text;
  std::string group;
  std::string role;
};

struct ClipboardTextGroup {
  std::string name;
  std::vector<ClipboardTextEntry> entries;
};

static std::string FlattenClipboardText(const char *source) {
  std::string result;
  const char *p = source ? source : "";
  while (*p) {
    int breakLength = 0;
    if (*p == '\\' && (p[1] == 'N' || p[1] == 'n'))
      breakLength = 2;
    else if (*p == '\r' && p[1] == '\n')
      breakLength = 2;
    else if (*p == '\r' || *p == '\n')
      breakLength = 1;
    if (breakLength) {
      while (!result.empty() && result.back() == ' ')
        result.pop_back();
      p += breakLength;
      while (*p == ' ')
        p++;
      if (!result.empty() && *p)
        result.push_back(' ');
    } else {
      result.push_back(*p++);
    }
  }
  return result;
}

static void CopyMarkersToClipboard(HWND hwnd, int formatOverride = -1) {
  SNM_SetIntConfigVar("projtimemode", 5);
  UpdateTimeline();

  WDL_PtrList_DOD<SNM_Actor> *actorsList = g_actors.Get();
  bool hasAnyActor = (actorsList && actorsList->GetSize() > 0);
  bool anyActorEnabled = false;
  bool hasDisabledActor = false;
  if (hasAnyActor) {
    for (int i = 0; i < actorsList->GetSize(); i++) {
      if (actorsList->Get(i)->IsEnabled())
        anyActorEnabled = true;
      else
        hasDisabledActor = true;
    }
  }

  std::vector<ClipboardMarkerEntry> markers;
  std::vector<ClipboardRegionEntry> regions;
  int enumIndex = 0;
  bool isRegion = false;
  double position = 0.0;
  double regionEnd = 0.0;
  const char *name = NULL;
  int number = 0;
  while ((enumIndex = EnumProjectMarkers2(NULL, enumIndex, &isRegion,
                                           &position, &regionEnd, &name, &number))) {
    if (isRegion)
      regions.push_back({position, regionEnd, number, name ? name : ""});
    else
      markers.push_back({position, name ? name : ""});
  }

  // If subtitle regions are currently hidden from the timeline, include hidden subtitle regions from g_pRegionSubs
  if (g_hideRegions) {
    WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
    for (int i = 0; i < subs->GetSize(); i++) {
      SNM_RegionSubtitle *sub = subs->Get(i);
      if (sub && sub->GetActor() && sub->GetActor()[0] && strcmp(sub->GetActor(), "?") != 0) {
        regions.push_back({sub->GetStartTime(), sub->GetEndTime(), -1000 - i, ""});
      }
    }
  }

  WDL_PtrList_DOD<SNM_RegionSubtitle> *subtitles = g_pRegionSubs.Get();
  std::vector<ClipboardTextEntry> entries;
  for (const ClipboardRegionEntry &region : regions) {
    std::vector<const ClipboardMarkerEntry *> insideMarkers;
    for (const ClipboardMarkerEntry &marker : markers) {
      if (marker.position >= region.start - 0.05 && marker.position <= region.end + 0.05)
        insideMarkers.push_back(&marker);
    }
    if (insideMarkers.empty())
      continue;

    SNM_RegionSubtitle *subtitle = NULL;
    if (region.number >= 0) {
      const int regionId = MakeMarkerRegionId(region.number, true);
      for (int i = 0; i < subtitles->GetSize(); i++) {
        SNM_RegionSubtitle *candidate = subtitles->Get(i);
        if (candidate && candidate->IsValid() && candidate->GetId() == regionId &&
            candidate->GetActor() && candidate->GetActor()[0] && strcmp(candidate->GetActor(), "?") != 0) {
          subtitle = candidate;
          break;
        }
      }
    } else {
      int subIdx = -1000 - region.number;
      if (subIdx >= 0 && subIdx < subtitles->GetSize())
        subtitle = subtitles->Get(subIdx);
    }
    if (!subtitle) {
      for (int i = 0; i < subtitles->GetSize(); i++) {
        SNM_RegionSubtitle *candidate = subtitles->Get(i);
        if (candidate && candidate->GetActor() && candidate->GetActor()[0] && strcmp(candidate->GetActor(), "?") != 0 &&
            fabs(candidate->GetStartTime() - region.start) < 0.005 &&
            fabs(candidate->GetEndTime() - region.end) < 0.005) {
          subtitle = candidate;
          break;
        }
      }
    }

    SNM_Actor *subActor = NULL;
    if (subtitle && subtitle->GetActor() && subtitle->GetActor()[0] && strcmp(subtitle->GetActor(), "?") != 0) {
      subActor = FindActor(subtitle->GetActor());
    }

    std::string role;
    std::string group;
    std::string text;

    if (subActor) {
      // Subtitle region with an associated actor
      if (anyActorEnabled && !subActor->IsEnabled())
        continue;

      role = subActor->GetName();
      if (subActor->HasLinkedActor())
        group = subActor->GetLinkedActorName();
      text = FlattenClipboardText(subtitle->GetNotes());
    } else {
      // Personal user region: separate structure, always copied, no actor/role grouping
      text = FlattenClipboardText(region.text.c_str());
    }

    for (const ClipboardMarkerEntry *marker : insideMarkers) {
      std::string markerText = FlattenClipboardText(marker->text.c_str());
      if (!markerText.empty()) {
        if (!text.empty()) {
          text.append(" (");
          text.append(markerText);
          text.push_back(')');
        } else {
          text = markerText;
        }
      }
    }

    if (text.empty()) {
      if (!region.text.empty())
        text = FlattenClipboardText(region.text.c_str());
      else
        text = "Регион " + std::to_string(region.number);
    }

    entries.push_back({region.start, region.end, text, group, role});
  }

  std::vector<ClipboardTextEntry> personalRegions;
  std::vector<ClipboardTextEntry> unlinked;
  std::vector<ClipboardTextGroup> groups;
  for (const ClipboardTextEntry &entry : entries) {
    if (entry.role.empty()) {
      personalRegions.push_back(entry);
      continue;
    }
    if (entry.group.empty()) {
      unlinked.push_back(entry);
      continue;
    }
    ClipboardTextGroup *target = NULL;
    for (ClipboardTextGroup &group : groups) {
      if (group.name == entry.group) {
        target = &group;
        break;
      }
    }
    if (!target) {
      groups.push_back({entry.group, {}});
      target = &groups.back();
    }
    target->entries.push_back(entry);
  }

  auto sortByTime = [](std::vector<ClipboardTextEntry> &items) {
    std::sort(items.begin(), items.end(), [](const ClipboardTextEntry &a,
                                             const ClipboardTextEntry &b) {
      if (a.start != b.start)
        return a.start < b.start;
      return a.end < b.end;
    });
  };
  sortByTime(personalRegions);
  sortByTime(unlinked);
  for (ClipboardTextGroup &group : groups)
    sortByTime(group.entries);

  WDL_FastString output;
  auto appendEntries = [&output, formatOverride](const std::vector<ClipboardTextEntry> &items) {
    for (const ClipboardTextEntry &entry : items) {
      char startBuffer[64];
      char endBuffer[64];
      FormatSubTime(entry.start, startBuffer, sizeof(startBuffer), formatOverride);
      FormatSubTime(entry.end, endBuffer, sizeof(endBuffer), formatOverride);

      if (!entry.role.empty()) {
        if (!entry.text.empty()) {
          output.AppendFormatted(512, "%s-%s (%s) %s\n", startBuffer, endBuffer,
                                 entry.role.c_str(), entry.text.c_str());
        } else {
          output.AppendFormatted(512, "%s-%s (%s)\n", startBuffer, endBuffer,
                                 entry.role.c_str());
        }
      } else {
        if (!entry.text.empty()) {
          output.AppendFormatted(512, "%s-%s %s\n", startBuffer, endBuffer,
                                 entry.text.c_str());
        } else {
          output.AppendFormatted(512, "%s-%s\n", startBuffer, endBuffer);
        }
      }
    }
  };

  bool hasRoles = (!unlinked.empty() || !groups.empty());
  bool hasPersonal = !personalRegions.empty();

  if (hasRoles) {
    appendEntries(unlinked);
    for (const ClipboardTextGroup &group : groups) {
      if (output.GetLength() > 0)
        output.Append("\n");
      output.AppendFormatted(256, "%s\n", group.name.c_str());
      appendEntries(group.entries);
    }
  }

  if (hasPersonal) {
    if (output.GetLength() > 0)
      output.Append("\n");
    if (hasRoles)
      output.Append("Регионы\n");
    appendEntries(personalRegions);
  }

  if (output.GetLength() > 0 && output.Get()[output.GetLength() - 1] == '\n')
    output.DeleteSub(output.GetLength() - 1, 1);

  if (output.GetLength() > 0)
    CF_SetClipboard(output.Get());
  else
    MessageBox(hwnd,
               __LOCALIZE("Нет маркеров внутри регионов.", "sws_DLG_152"),
               __LOCALIZE("ReNotes", "sws_DLG_152"),
               MB_OK);
}

static WDL_DLGRET LinkActorDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  if (INT_PTR r = SNM_HookThemeColorsMessage(hwndDlg, uMsg, wParam, lParam))
    return r;

  switch (uMsg) {
    case WM_INITDIALOG: {
      SetWindowLongPtr(hwndDlg, GWLP_USERDATA, lParam);
      SetDlgItemText(hwndDlg, -1, __LOCALIZE("Имя актёра:", "sws_DLG_152"));
      SetDlgItemText(hwndDlg, IDCANCEL, __LOCALIZE("Отменить", "sws_DLG_152"));
      HWND combo = GetDlgItem(hwndDlg, IDC_COMBO);
      WDL_UTF8_HookComboBox(combo);
      WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
      WDL_PtrList<const char> added;
      for (int i = 0; i < actors->GetSize(); i++) {
        SNM_Actor *a = actors->Get(i);
        if (a->HasLinkedActor()) {
          const char *name = a->GetLinkedActorName();
          bool found = false;
          for (int j = 0; j < added.GetSize(); j++) {
            if (!strcmp(added.Get(j), name)) {
              found = true;
              break;
            }
          }
          if (!found) {
            SendMessage(combo, CB_ADDSTRING, 0, (LPARAM) name);
            added.Add(name);
          }
        }
      }
      SetWindowPos(hwndDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDOK: {
          char *buf = (char *) GetWindowLongPtr(hwndDlg, GWLP_USERDATA);
          GetDlgItemText(hwndDlg, IDC_COMBO, buf, 256);
          EndDialog(hwndDlg, 1);
          return 0;
        }
        case IDCANCEL:
          EndDialog(hwndDlg, 0);
          return 0;
      }
      break;
  }
  return 0;
}

void NotesWnd::OnCommand(WPARAM wParam, LPARAM lParam)
{
	const int command = LOWORD(wParam);
	if (command >= ROLE_PROFILE_MSG_BASE &&
	    command < ROLE_PROFILE_MSG_BASE + (int)g_roleProfileMenuEntries.size()) {
		const RoleProfileMenuEntry &profile =
		  g_roleProfileMenuEntries[command - ROLE_PROFILE_MSG_BASE];
		if (ImportRoleProfile(profile.name.c_str(), profile.path.c_str())) {
			UpdateRegionColors();
			RefreshActorList();
			ForceUpdateRgnSub();
			RefreshGUI();
			SyncSubtitleItemsTrack(false);
			MarkProjectDirty(NULL);
		} else {
			MessageBox(m_hwnd,
			           __LOCALIZE("Не удалось импортировать профиль ролёвки.", "sws_DLG_152"),
			           __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
			           MB_OK);
		}
		return;
	}
	switch (LOWORD(wParam))
	{
		case IDC_EDIT1:
		case IDC_EDIT2:
			if (HIWORD(wParam)==EN_CHANGE && !m_settingText)
				SaveCurrentText(g_notesType, MarkProjectDirty==NULL); // MarkProjectDirty() avail. since v4.55pre2
			break;
		case WRAP_MSG:
			SetWrapText(!g_wrapText);
			break;
		case SAVE_GLOBAL_NOTES_MSG:
			WriteGlobalNotesToFile();
			RefreshGUI();
			break;
		case BTNID_LOCK:
			if (!HIWORD(wParam))
				ToggleLock();
			break;
		case BTNID_IMPORT_SUB:
			ImportSubTitleFile(NULL);
			break;
		case BTNID_CLEAR_SUBS:
      if (!HIWORD(wParam))
        ClearAllSubtitlesAction(NULL);
      break;
    case BTNID_VIDEO_OVERLAY:
    case TOGGLE_VIDEO_OVERLAY_MSG:
      ToggleVideoOverlayAction(NULL);
      break;
    case TOGGLE_VIDEO_OVERLAY_ACTOR_MSG:
      g_videoOverlayIncludeActor = !g_videoOverlayIncludeActor;
      SyncSubtitlesToVideoOverlay();
      RefreshGUI();
      MarkProjectDirty(NULL);
      break;
    case VIDEO_COLOR_NONE_MSG:
    case VIDEO_COLOR_ROLE_MSG:
    case VIDEO_COLOR_ALL_MSG: {
      if (LOWORD(wParam) == VIDEO_COLOR_NONE_MSG) g_videoOverlayColorMode = 0;
      else if (LOWORD(wParam) == VIDEO_COLOR_ALL_MSG) g_videoOverlayColorMode = 2;
      else g_videoOverlayColorMode = 1;
      WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlayColorMode",
                                g_videoOverlayColorMode == 0 ? "0" : (g_videoOverlayColorMode == 2 ? "2" : "1"),
                                g_SNM_IniFn.Get());
      MediaTrack *vtr = FindVideoTrack();
      if (vtr) {
        int fx = FindVideoSubtitleFX(vtr);
        if (fx >= 0) ApplyVideoSubtitleFXParams(vtr, fx);
      }
      SyncSubtitlesToVideoOverlay();
      RefreshGUI();
      MarkProjectDirty(NULL);
      break;
    }
    case VIDEO_SCALE_80_MSG:
    case VIDEO_SCALE_100_MSG:
    case VIDEO_SCALE_125_MSG:
    case VIDEO_SCALE_150_MSG: {
      if (LOWORD(wParam) == VIDEO_SCALE_80_MSG) g_videoOverlayScale = 0.80;
      else if (LOWORD(wParam) == VIDEO_SCALE_125_MSG) g_videoOverlayScale = 1.25;
      else if (LOWORD(wParam) == VIDEO_SCALE_150_MSG) g_videoOverlayScale = 1.50;
      else g_videoOverlayScale = 1.00;
      MediaTrack *vtr = FindVideoTrack();
      if (vtr) {
        int fx = FindVideoSubtitleFX(vtr);
        if (fx >= 0) ApplyVideoSubtitleFXParams(vtr, fx);
      }
      SyncSubtitlesToVideoOverlay();
      RefreshGUI();
      MarkProjectDirty(NULL);
      break;
    }
    case VIDEO_POS_BOTTOM_MSG:
    case VIDEO_POS_VERY_BOTTOM_MSG:
    case VIDEO_POS_CENTER_MSG:
    case VIDEO_POS_TOP_MSG: {
      if (LOWORD(wParam) == VIDEO_POS_VERY_BOTTOM_MSG) g_videoOverlayPosY = 0.94;
      else if (LOWORD(wParam) == VIDEO_POS_CENTER_MSG) g_videoOverlayPosY = 0.50;
      else if (LOWORD(wParam) == VIDEO_POS_TOP_MSG) g_videoOverlayPosY = 0.12;
      else g_videoOverlayPosY = 0.88;
      MediaTrack *vtr = FindVideoTrack();
      if (vtr) {
        int fx = FindVideoSubtitleFX(vtr);
        if (fx >= 0) ApplyVideoSubtitleFXParams(vtr, fx);
      }
      SyncSubtitlesToVideoOverlay();
      RefreshGUI();
      MarkProjectDirty(NULL);
      break;
    }
    case VIDEO_BG_NONE_MSG:
    case VIDEO_BG_LIGHT_MSG:
    case VIDEO_BG_MEDIUM_MSG:
    case VIDEO_BG_DARK_MSG: {
      if (LOWORD(wParam) == VIDEO_BG_NONE_MSG) g_videoOverlayBgAlpha = 0.00;
      else if (LOWORD(wParam) == VIDEO_BG_LIGHT_MSG) g_videoOverlayBgAlpha = 0.30;
      else if (LOWORD(wParam) == VIDEO_BG_DARK_MSG) g_videoOverlayBgAlpha = 0.80;
      else g_videoOverlayBgAlpha = 0.55;
      MediaTrack *vtr = FindVideoTrack();
      if (vtr) {
        int fx = FindVideoSubtitleFX(vtr);
        if (fx >= 0) ApplyVideoSubtitleFXParams(vtr, fx);
      }
      SyncSubtitlesToVideoOverlay();
      RefreshGUI();
      MarkProjectDirty(NULL);
      break;
    }
    case OPEN_VIDEO_FX_MSG: {
      MediaTrack *vtr = FindVideoTrack(true);
      if (vtr) {
        int fx = EnsureVideoSubtitleFX(vtr, true);
        if (fx >= 0)
          TrackFX_SetOpen(vtr, fx, true);
      } else {
        MessageBox(m_hwnd,
          __LOCALIZE("Видео-трек не найден! Пожалуйста, добавьте видео-файл на дорожку.", "sws_DLG_152"),
          __LOCALIZE("ReNotes - Субтитры на видео", "sws_DLG_152"),
          MB_OK);
      }
      break;
    }
    case REFRESH_VIDEO_OVERLAY_MSG:
      RefreshVideoOverlayAction(NULL);
      break;
    case COLORED_REGIONS_MSG:
      ToggleColoredRegions(NULL);
      break;
    case HIDE_REGIONS_MSG:
      ToggleHideRegions(NULL);
      break;
    case HIDE_ACTOR_LIST_MSG:
      ToggleHideActorList(NULL);
      break;
    case DISPLAY_ACTOR_PREFIX_MSG:
      ToggleDisplayActorInPrefix(NULL);
      break;
    case ENABLE_ALL_MSG:
      EnableAllActors(NULL);
      break;
    case DISABLE_ALL_MSG:
      DisableAllActors(NULL);
      break;
    case COPY_MARKERS_MSG:
      CopyMarkersToClipboard(GetHWND());
      break;
    case COPY_MARKERS_WITH_FRAMES_MSG:
      CopyMarkersToClipboard(GetHWND(), SNM_TIME_FORMAT_FRAMES);
      break;
    case TIME_FORMAT_NO_FRAMES_MSG:
      g_copyTimeFormat = SNM_TIME_FORMAT_NO_FRAMES;
      WritePrivateProfileString(NOTES_INI_SEC, "CopyTimeFormat", "0", g_SNM_IniFn.Get());
      break;
    case TIME_FORMAT_FRAMES_MSG:
      g_copyTimeFormat = SNM_TIME_FORMAT_FRAMES;
      WritePrivateProfileString(NOTES_INI_SEC, "CopyTimeFormat", "1", g_SNM_IniFn.Get());
      break;
    case TIME_FORMAT_MS_MSG:
      g_copyTimeFormat = SNM_TIME_FORMAT_MS;
      WritePrivateProfileString(NOTES_INI_SEC, "CopyTimeFormat", "2", g_SNM_IniFn.Get());
      break;
    case CHECK_RENOTES_UPDATES_MSG:
      CheckReNotesUpdates(true);
      break;
    case AUTO_CHECK_UPDATES_MSG:
      ToggleAutoCheckUpdatesAction(NULL);
      break;
    case COPY_CURRENT_PHRASE_MSG: {
      if (m_overlappingRegionIds.GetSize() <= 0) break;
      WDL_FastString output;
      for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
        SNM_RegionSubtitle *sub = NULL;
        double pos, endPos;
        if (g_hideRegions) {
          int subIdx = m_overlappingRegionIds.Get()[i];
          sub = g_pRegionSubs.Get()->Get(subIdx);
          if (!sub) continue;
          pos = sub->GetStartTime();
          endPos = sub->GetEndTime();
        } else {
          int regId = m_overlappingRegionIds.Get()[i];
          for (int j = 0; j < g_pRegionSubs.Get()->GetSize(); j++) {
            if (g_pRegionSubs.Get()->Get(j)->GetId() == regId) {
              sub = g_pRegionSubs.Get()->Get(j);
              break;
            }
          }
          if (!sub) continue;
          if (EnumMarkerRegionById(NULL, regId, NULL, &pos, &endPos, NULL, NULL, NULL) < 0)
            continue;
        }
        {
            char startBuf[32], endBuf[32];
            FormatSubTime(pos, startBuf, sizeof(startBuf));
            FormatSubTime(endPos, endBuf, sizeof(endBuf));

            WDL_FastString text(sub->GetNotes());
            for (int p = 0; p < text.GetLength();) {
              char c = text.Get()[p];
              bool isBreak = false;
              int breakLen = 0;
              if (c == '\\' && p + 1 < text.GetLength() && (text.Get()[p + 1] == 'N' || text.Get()[p + 1] == 'n')) {
                isBreak = true;
                breakLen = 2;
              } else if (c == '\r' && p + 1 < text.GetLength() && text.Get()[p + 1] == '\n') {
                isBreak = true;
                breakLen = 2;
              } else if (c == '\n' || c == '\r') {
                isBreak = true;
                breakLen = 1;
              }
              if (isBreak) {
                int start = p;
                while (start > 0 && text.Get()[start - 1] == ' ') start--;
                int end = p + breakLen;
                while (end < text.GetLength() && text.Get()[end] == ' ') end++;
                text.DeleteSub(start, end - start);
                text.Insert(" ", start);
                p = start + 1;
              } else {
                p++;
              }
            }

            if (output.GetLength() > 0)
              output.Append("\n");
            output.AppendFormatted(512, "%s-%s %s", startBuf, endBuf, text.Get());
        }
      }
      if (output.GetLength() > 0)
        CF_SetClipboard(output.Get());
    }
    break;
    case CREATE_SUBTITLE_ITEMS_TRACK_MSG:
      SyncSubtitleItemsTrack(true);
      break;
    case COPY_ROLE_DISTRIBUTION_MSG:
      CopyRoleDistributionAction(NULL);
			break;
		case CMBID_TYPE:
			if (HIWORD(wParam)==CBN_SELCHANGE) {
				SetType(m_cbType.GetCurSel2());
				if (!g_locked)
					SetFocus(m_edit);
			}
			break;
    case CMBID_REGION:
      if (HIWORD(wParam) == CBN_SELCHANGE) {
        int selIdx = m_cbRegion.GetCurSel2();
        if (selIdx >= 0 && selIdx < m_overlappingRegionIds.GetSize()) {
          g_lastMarkerRegionId = m_overlappingRegionIds.Get()[selIdx];
          if (g_hideRegions) {
            SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(g_lastMarkerRegionId);
            if (sub)
              SetText(sub->GetNotes());
          } else {
            for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
              SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(i);
              if (sub->GetId() == g_lastMarkerRegionId) {
                SetText(sub->GetNotes());
                break;
              }
            }
          }
          RefreshGUI();
          if (!g_locked)
            SetFocus(m_edit);
        }
      }
      break;
    case LINK_ACTOR_MSG:
      if (m_contextMenuActor) {
        char reply[256] = "";
        if (DialogBoxParam(g_hInst,
                           MAKEINTRESOURCE(IDD_LINK_ACTOR),
                           m_hwnd,
                           LinkActorDlgProc,
                           (LPARAM)reply) && reply[0]) {
          m_contextMenuActor->SetLinkedActorName(reply);
          WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
          for (int i = 0; i < actors->GetSize(); i++) {
            SNM_Actor *a = actors->Get(i);
            if (a != m_contextMenuActor && a->HasLinkedActor()
              && !strcmp(a->GetLinkedActorName(), reply) && a->HasCustomColor()) {
              m_contextMenuActor->SetColor(a->GetColor());
              m_contextMenuActor->SetHasCustomColor(true);
              break;
            }
          }
          UpdateRegionColors();
          RefreshActorList();
          SyncSubtitleItemsTrack(false);
          SaveActiveRoleProfile();
          MarkProjectDirty(NULL);
        }
      }
      break;
    case UNLINK_ACTOR_MSG:
      if (m_contextMenuActor) {
        m_contextMenuActor->SetLinkedActorName("");
        m_contextMenuActor->SetHasCustomColor(false);
        m_contextMenuActor->SetColor(GenerateActorColor(m_contextMenuActor->GetName()));
        UpdateRegionColors();
        RefreshActorList();
        SyncSubtitleItemsTrack(false);
        SaveActiveRoleProfile();
        MarkProjectDirty(NULL);
      }
      break;
    case SELECT_LINKED_ACTOR_ROLES_MSG:
      if (m_contextMenuActor && m_contextMenuActor->HasLinkedActor()) {
        const char *linkedActor = m_contextMenuActor->GetLinkedActorName();
        WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
        for (int i = 0; i < actors->GetSize(); i++) {
          SNM_Actor *actor = actors->Get(i);
          const bool enable = actor->HasLinkedActor()
            && !strcmp(actor->GetLinkedActorName(), linkedActor);
          if (actor->IsEnabled() == enable)
            continue;
          actor->SetEnabled(enable);
          if (enable)
            RecreateActorRegions(actor->GetName());
          else
            DeleteActorRegions(actor->GetName());
        }
        SyncSubtitleItemsTrack(false);
        UpdateTimeline();
        RefreshActorList();
        if (g_videoOverlayEnabled)
          SyncSubtitlesToVideoOverlay();
        MarkProjectDirty(NULL);
      }
      break;
    case HIGHLIGHT_ACTOR_ROLES_MSG:
      if (m_contextMenuActor) {
        const char *target = m_contextMenuActor->HasLinkedActor()
          ? m_contextMenuActor->GetLinkedActorName()
          : m_contextMenuActor->GetName();
        const char *curHighlight = g_highlightedActor.Get()->Get();
        if (curHighlight && *curHighlight && !strcmp(curHighlight, target)) {
          g_highlightedActor.Get()->Set("");
        } else {
          g_highlightedActor.Get()->Set(target);
          if (!g_coloredRegions)
            g_coloredRegions = true;
          WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
          for (int i = 0; i < actors->GetSize(); i++) {
            SNM_Actor *a = actors->Get(i);
            if (!a->IsEnabled()) {
              a->SetEnabled(true);
              RecreateActorRegions(a->GetName());
            }
          }
        }
        UpdateRegionColors();
        UpdateTimeline();
        RefreshActorList();
        SyncSubtitleItemsTrack(false);
        RefreshGUI();
        MarkProjectDirty(NULL);
      }
      break;
    case CLEAR_HIGHLIGHT_ROLES_MSG:
      ClearHighlightRolesAction(NULL);
      break;
    case DELETE_ROLE_MSG:
      if (m_contextMenuActor) {
        const std::string name = m_contextMenuActor->GetName();
        DeleteActorRegions(name.c_str());
        WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
        int idx = actors->Find(m_contextMenuActor);
        if (idx >= 0)
          actors->Delete(idx, true);
        m_contextMenuActor = NULL;
        if (SNM_Actor *mem = FindActorInList(g_importedRoleActors.Get(), name.c_str()))
          g_importedRoleActors.Get()->Delete(g_importedRoleActors.Get()->Find(mem), true);
        UpdateTimeline();
        RefreshActorList();
        SyncSubtitleItemsTrack(false);
        SaveActiveRoleProfile();
        MarkProjectDirty(NULL);
      }
      break;
    case DELETE_UNUSED_ROLES_MSG: {
      WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
      WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
      bool changed = false;
      for (int i = actors->GetSize() - 1; i >= 0; i--) {
        SNM_Actor *actor = actors->Get(i);
        bool hasSubtitle = false;
        for (int j = 0; j < subs->GetSize(); j++) {
          if (!strcmp(subs->Get(j)->GetActor(), actor->GetName())) {
            hasSubtitle = true;
            break;
          }
        }
        if (!hasSubtitle) {
          const std::string name = actor->GetName();
          if (SNM_Actor *mem = FindActorInList(g_importedRoleActors.Get(), name.c_str()))
            g_importedRoleActors.Get()->Delete(g_importedRoleActors.Get()->Find(mem), true);
          actors->Delete(i, true);
          changed = true;
        }
      }
      if (changed) {
        RefreshActorList();
        SyncSubtitleItemsTrack(false);
        SaveActiveRoleProfile();
        MarkProjectDirty(NULL);
      }
      break;
    }
    case CLEAR_ALL_ROLES_MSG:
      if (MessageBox(m_hwnd,
                     __LOCALIZE("Вы уверены, что хотите удалить все роли?", "sws_DLG_152"),
                     __LOCALIZE("ReNotes", "sws_DLG_152"),
                     MB_YESNO) == IDYES) {
        g_actors.Get()->Empty(true);
        g_importedRoleActors.Get()->Empty(true);
        g_highlightedActor.Get()->Set("");
        RefreshActorList();
        SyncSubtitleItemsTrack(false);
        SaveActiveRoleProfile();
        MarkProjectDirty(NULL);
      }
      break;
    case CHANGE_COLOR_MSG:
      if (m_contextMenuActor) {
        int color = m_contextMenuActor->GetEffectiveColor() & 0xFFFFFF;
        if (GR_SelectColor(m_hwnd, &color)) {
          color |= 0x1000000;
          if (m_contextMenuActor->HasLinkedActor()) {
            const char *linked = m_contextMenuActor->GetLinkedActorName();
            WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
            for (int i = 0; i < actors->GetSize(); i++) {
              SNM_Actor *a = actors->Get(i);
              if (a->HasLinkedActor() && !strcmp(a->GetLinkedActorName(), linked)) {
                a->SetColor(color);
                a->SetHasCustomColor(true);
              }
            }
          } else {
            m_contextMenuActor->SetColor(color);
            m_contextMenuActor->SetHasCustomColor(true);
          }
          UpdateRegionColors();
          RefreshActorList();
          SyncSubtitleItemsTrack(false);
          SaveActiveRoleProfile();
          MarkProjectDirty(NULL);
        }
      }
      break;
    case EXPORT_ROLES_MSG: {
      WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
      bool hasData = (actors->GetSize() > 0 || g_importedRoleActors.Get()->GetSize() > 0);
      if (!hasData) {
        MessageBox(m_hwnd,
                   __LOCALIZE("Ничего не найдено для экспорта.", "sws_DLG_152"),
                   __LOCALIZE("ReNotes", "sws_DLG_152"),
                   MB_OK);
        break;
      }
      char fn[SNM_MAX_PATH] = "";
      SaveActiveRoleProfile();
      if (BrowseForSaveFile(
        __LOCALIZE("ReNotes - Экспортировать ролёвку", "sws_DLG_152"),
        g_lastRolesFn,
        NULL,
        SNM_ROLES_EXT_LIST,
        fn,
        sizeof(fn))) {
        lstrcpyn(g_lastRolesFn, fn, sizeof(g_lastRolesFn));
        if (!ExportRolesFile(fn))
          MessageBox(m_hwnd,
                     __LOCALIZE("Не удалось экспортировать ролёвку.", "sws_DLG_152"),
                     __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                     MB_OK);
      }
    }
    break;
    case IMPORT_ROLES_MSG:
      if (char *fn = BrowseForFiles(
        __LOCALIZE("ReNotes - Импортировать ролёвку", "sws_DLG_152"),
        g_lastRolesFn,
        NULL,
        false,
        SNM_ROLES_EXT_LIST)) {
        lstrcpyn(g_lastRolesFn, fn, sizeof(g_lastRolesFn));
        if (ImportRolesFile(fn)) {
          UpdateRegionColors();
          RefreshActorList();
          ForceUpdateRgnSub();
          RefreshGUI();
          SyncSubtitleItemsTrack(false);
          MarkProjectDirty(NULL);
        } else
          MessageBox(m_hwnd,
                     __LOCALIZE("Некорректный файл ролёвки!", "sws_DLG_152"),
                     __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                     MB_OK);
        free(fn);
			}
			break;
		default:
			Main_OnCommand((int)wParam, (int)lParam);
			break;
	}
}

// bWantEdit ignored: no list view in there
bool NotesWnd::IsActive(bool bWantEdit) {
	return (IsValidWindow() && (GetForegroundWindow() == m_hwnd || GetFocus() == m_edit));
}

HMENU NotesWnd::OnContextMenu(int x, int y, bool* wantDefaultItems)
{
  HMENU hMenu = CreatePopupMenu();

  if (g_notesType == SNM_NOTES_RGN_SUB && m_actorListView) {
    HWND listHwnd = m_actorListView->GetHWND();
    RECT listRect;
    GetWindowRect(listHwnd, &listRect);
    POINT pt = {x, y};
    if (PtInRect(&listRect, pt)) {
      int iCol;
      SWS_ListItem *hitItem = m_actorListView->GetHitItem(x, y, &iCol);
      if (hitItem) {
        SNM_Actor *hitActor = (SNM_Actor *) hitItem;
        m_contextMenuActor = (hitActor && g_actors.Get()->Find(hitActor) >= 0) ? hitActor : NULL;
        if (m_contextMenuActor) {
          if (m_contextMenuActor->HasLinkedActor())
            AddToMenu(hMenu, __LOCALIZE("Отвязать актёра","sws_DLG_152"),
                      UNLINK_ACTOR_MSG,
                      -1,
                      false);
          else
            AddToMenu(hMenu,
                      __LOCALIZE("Привязать актёра", "sws_DLG_152"),
                      LINK_ACTOR_MSG,
                      -1,
                      false);
          AddToMenu(hMenu,
                    m_contextMenuActor->HasLinkedActor()
                      ? __LOCALIZE("Изменить цвет (актёр)", "sws_DLG_152")
                      : __LOCALIZE("Изменить цвет", "sws_DLG_152"),
                    CHANGE_COLOR_MSG,
                    -1,
                    false);
          const char *target = m_contextMenuActor->HasLinkedActor()
            ? m_contextMenuActor->GetLinkedActorName()
            : m_contextMenuActor->GetName();
          const char *curHighlight = g_highlightedActor.Get()->Get();
          bool isHighlighted = curHighlight && *curHighlight && !strcmp(curHighlight, target);

          if (m_contextMenuActor->HasLinkedActor()) {
            AddToMenu(hMenu,
                      __LOCALIZE("Включить только роли этого актёра", "sws_DLG_152"),
                      SELECT_LINKED_ACTOR_ROLES_MSG,
                      -1,
                      false);
            AddToMenu(hMenu,
                      __LOCALIZE("Выделить цветом роли этого актёра", "sws_DLG_152"),
                      HIGHLIGHT_ACTOR_ROLES_MSG,
                      -1,
                      false,
                      isHighlighted ? MFS_CHECKED : MFS_UNCHECKED);
          } else {
            AddToMenu(hMenu,
                      __LOCALIZE("Выделить эту роль цветом", "sws_DLG_152"),
                      HIGHLIGHT_ACTOR_ROLES_MSG,
                      -1,
                      false,
                      isHighlighted ? MFS_CHECKED : MFS_UNCHECKED);
          }
          AddToMenu(hMenu,
                    __LOCALIZE("Удалить эту роль", "sws_DLG_152"),
                    DELETE_ROLE_MSG,
                    -1,
                    false);
          AddToMenu(hMenu, SWS_SEPARATOR, 0);
	}
      }
      AddToMenu(hMenu,
                __LOCALIZE("Все ✓\tCtrl+Shift+A", "sws_DLG_152"),
                ENABLE_ALL_MSG,
                -1,
                false);
      AddToMenu(hMenu,
                __LOCALIZE("Все 🗙\tCtrl+Shift+D", "sws_DLG_152"),
                DISABLE_ALL_MSG,
                -1,
                false);
      if (g_highlightedActor.Get()->GetLength() > 0) {
        AddToMenu(hMenu,
                  __LOCALIZE("Сбросить выделение цветом", "sws_DLG_152"),
                  CLEAR_HIGHLIGHT_ROLES_MSG,
                  -1,
                  false);
      }
      AddToMenu(hMenu, SWS_SEPARATOR, 0);
      HMENU importMenu = CreatePopupMenu();
      RefreshRoleProfileEntries();
      if (g_roleProfileMenuEntries.empty()) {
        AddToMenu(importMenu,
                  __LOCALIZE("Нет сохранённых проектов", "sws_DLG_152"),
                  0,
                  -1,
                  false,
                  MF_GRAYED);
      } else {
        for (size_t i = 0; i < g_roleProfileMenuEntries.size(); i++) {
          const bool active = g_activeRoleProfile.Get()->GetLength() &&
            !_stricmp(g_activeRoleProfile.Get()->Get(), g_roleProfileMenuEntries[i].name.c_str());
          AddToMenu(importMenu,
                    g_roleProfileMenuEntries[i].name.c_str(),
                    ROLE_PROFILE_MSG_BASE + (int)i,
                    -1,
                    false,
                    active ? MFS_CHECKED : MFS_UNCHECKED);
        }
        AddToMenu(importMenu, SWS_SEPARATOR, 0);
      }
      AddToMenu(importMenu,
                __LOCALIZE("Из файла...\tCtrl+Shift+I", "sws_DLG_152"),
                IMPORT_ROLES_MSG,
                -1,
                false);
      AddSubMenu(hMenu,
                 importMenu,
                 __LOCALIZE("Импортировать ролёвку", "sws_DLG_152"));
      AddToMenu(hMenu,
                __LOCALIZE("Экспортировать ролёвку\tCtrl+Shift+E", "sws_DLG_152"),
                EXPORT_ROLES_MSG,
                -1,
                false);
      AddToMenu(hMenu, SWS_SEPARATOR, 0);
      AddToMenu(hMenu,
                __LOCALIZE("Скопировать ролёвку в буфер обмена\tCtrl+Shift+R", "sws_DLG_152"),
                COPY_ROLE_DISTRIBUTION_MSG,
                -1,
                false);
      AddToMenu(hMenu, SWS_SEPARATOR, 0);
      AddToMenu(hMenu,
                __LOCALIZE("Удалить роли без реплик", "sws_DLG_152"),
                DELETE_UNUSED_ROLES_MSG,
                -1,
                false);
      AddToMenu(hMenu,
                __LOCALIZE("Очистить все роли", "sws_DLG_152"),
                CLEAR_ALL_ROLES_MSG,
                -1,
                false);
      *wantDefaultItems = false;
      return hMenu;
    }
  }

  if (g_notesType == SNM_NOTES_RGN_SUB) {
    AddToMenu(hMenu,
              __LOCALIZE("Скопировать текущую фразу в буфер обмена\tCtrl+C", "sws_DLG_152"),
              COPY_CURRENT_PHRASE_MSG, -1, false,
              m_overlappingRegionIds.GetSize() > 0 ? MF_ENABLED : MF_GRAYED);
    AddToMenu(hMenu,
              __LOCALIZE("Скопировать маркеры в буфер обмена\tCtrl+M", "sws_DLG_152"),
              COPY_MARKERS_MSG,
              -1,
              false);

    HMENU timeFormatMenu = CreatePopupMenu();
    AddToMenu(timeFormatMenu,
              __LOCALIZE("Без кадров (16:17 / 1:16:17)", "sws_DLG_152"),
              TIME_FORMAT_NO_FRAMES_MSG,
              -1,
              false);
    AddToMenu(timeFormatMenu,
              __LOCALIZE("С кадрами (16:17(27) / 1:16:17(27))", "sws_DLG_152"),
              TIME_FORMAT_FRAMES_MSG,
              -1,
              false);
    AddToMenu(timeFormatMenu,
              __LOCALIZE("С миллисекундами (16:22.504 / 1:16:22.504)", "sws_DLG_152"),
              TIME_FORMAT_MS_MSG,
              -1,
              false);

    CheckMenuItem(timeFormatMenu, TIME_FORMAT_NO_FRAMES_MSG, MF_BYCOMMAND | (g_copyTimeFormat == SNM_TIME_FORMAT_NO_FRAMES ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(timeFormatMenu, TIME_FORMAT_FRAMES_MSG, MF_BYCOMMAND | (g_copyTimeFormat == SNM_TIME_FORMAT_FRAMES ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(timeFormatMenu, TIME_FORMAT_MS_MSG, MF_BYCOMMAND | (g_copyTimeFormat == SNM_TIME_FORMAT_MS ? MF_CHECKED : MF_UNCHECKED));

    AddSubMenu(hMenu,
               timeFormatMenu,
               __LOCALIZE("Формат времени маркеров", "sws_DLG_152"));

    AddToMenu(hMenu,
              __LOCALIZE("Создать дорожку с репликами", "sws_DLG_152"),
              CREATE_SUBTITLE_ITEMS_TRACK_MSG,
              -1,
              false);

    AddToMenu(hMenu,
              __LOCALIZE("Отображать субтитры на видео (хардсаб)\tV", "sws_DLG_152"),
              TOGGLE_VIDEO_OVERLAY_MSG,
              -1,
              false);
    if (g_videoOverlayEnabled)
      CheckMenuItem(hMenu, TOGGLE_VIDEO_OVERLAY_MSG, MF_BYCOMMAND | MF_CHECKED);

    HMENU videoSubMenu = CreatePopupMenu();
    AddToMenu(videoSubMenu,
              __LOCALIZE("Показывать имя роли ([Роль])", "sws_DLG_152"),
              TOGGLE_VIDEO_OVERLAY_ACTOR_MSG,
              -1,
              false);
    if (g_videoOverlayIncludeActor)
      CheckMenuItem(videoSubMenu, TOGGLE_VIDEO_OVERLAY_ACTOR_MSG, MF_BYCOMMAND | MF_CHECKED);

    HMENU colorModeMenu = CreatePopupMenu();
    AddToMenu(colorModeMenu, __LOCALIZE("Выключен (белый)", "sws_DLG_152"), VIDEO_COLOR_NONE_MSG, -1, false);
    AddToMenu(colorModeMenu, __LOCALIZE("Только имя роли ([Роль])", "sws_DLG_152"), VIDEO_COLOR_ROLE_MSG, -1, false);
    AddToMenu(colorModeMenu, __LOCALIZE("Вся реплика целиком", "sws_DLG_152"), VIDEO_COLOR_ALL_MSG, -1, false);
    if (g_videoOverlayColorMode == 0) CheckMenuItem(colorModeMenu, VIDEO_COLOR_NONE_MSG, MF_BYCOMMAND | MF_CHECKED);
    else if (g_videoOverlayColorMode == 2) CheckMenuItem(colorModeMenu, VIDEO_COLOR_ALL_MSG, MF_BYCOMMAND | MF_CHECKED);
    else CheckMenuItem(colorModeMenu, VIDEO_COLOR_ROLE_MSG, MF_BYCOMMAND | MF_CHECKED);
    AddSubMenu(videoSubMenu, colorModeMenu, __LOCALIZE("Цвет роли", "sws_DLG_152"));

    AddToMenu(videoSubMenu, SWS_SEPARATOR, 0);

    HMENU scaleMenu = CreatePopupMenu();
    AddToMenu(scaleMenu, "80% (Маленький)", VIDEO_SCALE_80_MSG, -1, false);
    AddToMenu(scaleMenu, "100% (Стандартный)", VIDEO_SCALE_100_MSG, -1, false);
    AddToMenu(scaleMenu, "125% (Большой)", VIDEO_SCALE_125_MSG, -1, false);
    AddToMenu(scaleMenu, "150% (Очень большой)", VIDEO_SCALE_150_MSG, -1, false);
    if (fabs(g_videoOverlayScale - 0.80) < 0.05) CheckMenuItem(scaleMenu, VIDEO_SCALE_80_MSG, MF_BYCOMMAND | MF_CHECKED);
    else if (fabs(g_videoOverlayScale - 1.25) < 0.05) CheckMenuItem(scaleMenu, VIDEO_SCALE_125_MSG, MF_BYCOMMAND | MF_CHECKED);
    else if (fabs(g_videoOverlayScale - 1.50) < 0.05) CheckMenuItem(scaleMenu, VIDEO_SCALE_150_MSG, MF_BYCOMMAND | MF_CHECKED);
    else CheckMenuItem(scaleMenu, VIDEO_SCALE_100_MSG, MF_BYCOMMAND | MF_CHECKED);
    AddSubMenu(videoSubMenu, scaleMenu, __LOCALIZE("Масштаб шрифта", "sws_DLG_152"));

    HMENU posMenu = CreatePopupMenu();
    AddToMenu(posMenu, __LOCALIZE("Внизу (88%)", "sws_DLG_152"), VIDEO_POS_BOTTOM_MSG, -1, false);
    AddToMenu(posMenu, __LOCALIZE("В самом низу (94%)", "sws_DLG_152"), VIDEO_POS_VERY_BOTTOM_MSG, -1, false);
    AddToMenu(posMenu, __LOCALIZE("По центру (50%)", "sws_DLG_152"), VIDEO_POS_CENTER_MSG, -1, false);
    AddToMenu(posMenu, __LOCALIZE("Вверху (12%)", "sws_DLG_152"), VIDEO_POS_TOP_MSG, -1, false);
    if (fabs(g_videoOverlayPosY - 0.94) < 0.03) CheckMenuItem(posMenu, VIDEO_POS_VERY_BOTTOM_MSG, MF_BYCOMMAND | MF_CHECKED);
    else if (fabs(g_videoOverlayPosY - 0.50) < 0.05) CheckMenuItem(posMenu, VIDEO_POS_CENTER_MSG, MF_BYCOMMAND | MF_CHECKED);
    else if (fabs(g_videoOverlayPosY - 0.12) < 0.05) CheckMenuItem(posMenu, VIDEO_POS_TOP_MSG, MF_BYCOMMAND | MF_CHECKED);
    else CheckMenuItem(posMenu, VIDEO_POS_BOTTOM_MSG, MF_BYCOMMAND | MF_CHECKED);
    AddSubMenu(videoSubMenu, posMenu, __LOCALIZE("Положение по вертикали", "sws_DLG_152"));

    HMENU bgMenu = CreatePopupMenu();
    AddToMenu(bgMenu, __LOCALIZE("Без фона (0%)", "sws_DLG_152"), VIDEO_BG_NONE_MSG, -1, false);
    AddToMenu(bgMenu, __LOCALIZE("Слабый (30%)", "sws_DLG_152"), VIDEO_BG_LIGHT_MSG, -1, false);
    AddToMenu(bgMenu, __LOCALIZE("Средний (55%)", "sws_DLG_152"), VIDEO_BG_MEDIUM_MSG, -1, false);
    AddToMenu(bgMenu, __LOCALIZE("Плотный (80%)", "sws_DLG_152"), VIDEO_BG_DARK_MSG, -1, false);
    if (g_videoOverlayBgAlpha < 0.10) CheckMenuItem(bgMenu, VIDEO_BG_NONE_MSG, MF_BYCOMMAND | MF_CHECKED);
    else if (fabs(g_videoOverlayBgAlpha - 0.30) < 0.10) CheckMenuItem(bgMenu, VIDEO_BG_LIGHT_MSG, MF_BYCOMMAND | MF_CHECKED);
    else if (fabs(g_videoOverlayBgAlpha - 0.80) < 0.10) CheckMenuItem(bgMenu, VIDEO_BG_DARK_MSG, MF_BYCOMMAND | MF_CHECKED);
    else CheckMenuItem(bgMenu, VIDEO_BG_MEDIUM_MSG, MF_BYCOMMAND | MF_CHECKED);
    AddSubMenu(videoSubMenu, bgMenu, __LOCALIZE("Фон под текстом", "sws_DLG_152"));

    AddToMenu(videoSubMenu, SWS_SEPARATOR, 0);
    AddToMenu(videoSubMenu,
              __LOCALIZE("Открыть настройки Video Processor...", "sws_DLG_152"),
              OPEN_VIDEO_FX_MSG,
              -1,
              false);
    AddToMenu(videoSubMenu,
              __LOCALIZE("Обновить субтитры на видео", "sws_DLG_152"),
              REFRESH_VIDEO_OVERLAY_MSG,
              -1,
              false);

    AddSubMenu(hMenu, videoSubMenu, __LOCALIZE("Настройки хардсаба на видео", "sws_DLG_152"));

    AddToMenu(hMenu, SWS_SEPARATOR, 0);
    AddToMenu(hMenu,
              __LOCALIZE("Отображать цветные регионы\tC", "sws_DLG_152"),
              COLORED_REGIONS_MSG,
              -1,
              false);
    if (g_coloredRegions)
      CheckMenuItem(hMenu, COLORED_REGIONS_MSG, MF_BYCOMMAND | MF_CHECKED);
    AddToMenu(hMenu,
              __LOCALIZE("Отображать привязанного актёра в префиксе\tP", "sws_DLG_152"),
              DISPLAY_ACTOR_PREFIX_MSG,
              -1,
              false);
    if (g_displayActorInPrefix)
      CheckMenuItem(hMenu, DISPLAY_ACTOR_PREFIX_MSG, MF_BYCOMMAND | MF_CHECKED);

    AddToMenu(hMenu, SWS_SEPARATOR, 0);
    AddToMenu(hMenu,
              __LOCALIZE("Скрыть ролёвку\tA", "sws_DLG_152"),
              HIDE_ACTOR_LIST_MSG,
              -1,
              false);
    if (g_hideActorList)
      CheckMenuItem(hMenu, HIDE_ACTOR_LIST_MSG, MF_BYCOMMAND | MF_CHECKED);
    AddToMenu(hMenu,
              __LOCALIZE("Скрыть регионы\tR", "sws_DLG_152"),
              HIDE_REGIONS_MSG,
              -1,
              false);
    if (g_hideRegions)
      CheckMenuItem(hMenu, HIDE_REGIONS_MSG, MF_BYCOMMAND | MF_CHECKED);
    if (g_highlightedActor.Get()->GetLength() > 0) {
      AddToMenu(hMenu,
                __LOCALIZE("Сбросить выделение ролей цветом", "sws_DLG_152"),
                CLEAR_HIGHLIGHT_ROLES_MSG,
                -1,
                false);
    }
  }

  if (g_notesType == SNM_NOTES_GLOBAL)
    AddToMenu(hMenu, __LOCALIZE("Сохранить глобальные заметки", "sws_DLG_152"), SAVE_GLOBAL_NOTES_MSG, -1, false);

  AddToMenu(hMenu, SWS_SEPARATOR, 0);
  AddToMenu(hMenu,
            __LOCALIZE("Проверить обновления ReNotes...", "sws_DLG_152"),
            CHECK_RENOTES_UPDATES_MSG,
            -1,
            false);
  AddToMenu(hMenu,
            __LOCALIZE("Автоматически проверять обновления", "sws_DLG_152"),
            AUTO_CHECK_UPDATES_MSG,
            -1,
            false);
  if (g_autoCheckUpdates)
    CheckMenuItem(hMenu, AUTO_CHECK_UPDATES_MSG, MF_BYCOMMAND | MF_CHECKED);

  return hMenu;
}

// OSX fix/workaround (SWELL bug?)
#ifdef _SNM_SWELL_ISSUES
void OSXForceTxtChangeJob::Perform() {
	if (NotesWnd* w = g_notesWndMgr.Get())
		SendMessage(w->GetHWND(), WM_COMMAND, MAKEWPARAM(IDC_EDIT1, EN_CHANGE), 0);
}
#endif


// returns: 
// -1 = catch and send to the control 
//  0 = pass-thru to main window (then -666 in SWS_DockWnd::keyHandler())
//  1 = eat
int NotesWnd::OnKey(MSG* _msg, int _iKeyState)
{
	if (_msg->message == WM_KEYDOWN) {
    bool editFocused = (_msg->hwnd == m_edit) && !g_locked;
    bool isRgnSub    = (g_notesType == SNM_NOTES_RGN_SUB);

    if (!editFocused && !_iKeyState && _msg->wParam == 'L') {
      OnCommand(BTNID_LOCK, 0);
      return 1;
	}

    if (isRgnSub) {
      if (_iKeyState == (LVKF_CONTROL | LVKF_SHIFT)) {
        switch (_msg->wParam) {
          case VK_DELETE: OnCommand(BTNID_CLEAR_SUBS, 0);           return 1;
          case 'I':       OnCommand(IMPORT_ROLES_MSG, 0);           return 1;
          case 'E':       OnCommand(EXPORT_ROLES_MSG, 0);           return 1;
          case 'R':       OnCommand(COPY_ROLE_DISTRIBUTION_MSG, 0); return 1;
          case 'A':       OnCommand(ENABLE_ALL_MSG, 0);             return 1;
          case 'D':       OnCommand(DISABLE_ALL_MSG, 0);            return 1;
          case 'M':       OnCommand(COPY_MARKERS_WITH_FRAMES_MSG, 0); return 1;
        }
      }
      if (_iKeyState == LVKF_CONTROL) {
        switch (_msg->wParam) {
          case 'I': OnCommand(BTNID_IMPORT_SUB, 0);  return 1;
          case 'M': OnCommand(COPY_MARKERS_MSG, 0);  return 1;
          case 'C':
            if (!editFocused) { OnCommand(COPY_CURRENT_PHRASE_MSG, 0); return 1; }
            break;
          case 'V':
#ifdef _WIN32
            if (IsClipboardFormatAvailable(CF_HDROP)) {
              if (OpenClipboard(m_hwnd)) {
                HANDLE hDrop = GetClipboardData(CF_HDROP);
                if (hDrop) {
                  HDROP hdrop = (HDROP)GlobalLock(hDrop);
                  if (hdrop) {
                    int iFiles = DragQueryFile(hdrop, 0xFFFFFFFF, NULL, 0);
                    bool imported = false;
                    bool rolesImported = false;
                    for (int i = 0; i < iFiles; i++) {
                      char fn[SNM_MAX_PATH] = "";
                      DragQueryFile(hdrop, i, fn, sizeof(fn));
                      if (HasFileExtension(fn, "ASS")) {
                        if (ImportAssFile(fn)) imported = true;
                        lstrcpyn(g_lastImportSubFn, fn, sizeof(g_lastImportSubFn));
                      } else if (HasFileExtension(fn, "SRT")) {
                        if (ImportSubRipFile(fn)) imported = true;
                        lstrcpyn(g_lastImportSubFn, fn, sizeof(g_lastImportSubFn));
                      } else if (HasFileExtension(fn, "ROLES")) {
                        if (ImportRolesFile(fn)) {
                          lstrcpyn(g_lastRolesFn, fn, sizeof(g_lastRolesFn));
                          rolesImported = true;
                        } else {
                          MessageBox(GetMainHwnd(),
                                     __LOCALIZE("Некорректный файл ролёвки!", "sws_DLG_152"),
                                     __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                                     MB_OK);
                        }
                      } else {
                        MessageBox(GetMainHwnd(),
                                   __LOCALIZE("Некорректный файл субтитров!", "sws_DLG_152"),
                                   __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                                   MB_OK);
                      }
                    }
                    GlobalUnlock(hDrop);
                    if (imported) {
                      MarkProjectDirty(NULL);
                      RefreshActorList();
                      SyncSubtitleItemsTrack(false);
                    }
                    if (rolesImported) {
                      UpdateRegionColors();
                      RefreshActorList();
                      ForceUpdateRgnSub();
                      RefreshGUI();
                      SyncSubtitleItemsTrack(false);
                      MarkProjectDirty(NULL);
                    }
                  }
                }
                CloseClipboard();
              }
              return 1;
            }
#endif
            break;
        }
      }
      if (!editFocused && !_iKeyState) {
        switch (_msg->wParam) {
          case 'C': OnCommand(COLORED_REGIONS_MSG,      0); return 1;
          case 'P': OnCommand(DISPLAY_ACTOR_PREFIX_MSG, 0); return 1;
          case 'R': OnCommand(HIDE_REGIONS_MSG,         0); return 1;
          case 'A': OnCommand(HIDE_ACTOR_LIST_MSG,      0); return 1;
          case 'V': OnCommand(TOGGLE_VIDEO_OVERLAY_MSG, 0); return 1;
        }
      }
    }
  }

  if (g_locked)
	{
		_msg->hwnd = m_hwnd; // redirect to main window
    return 0;
	}
	else if(_msg->hwnd == m_edit)
	{
#ifdef _SNM_SWELL_ISSUES
		// Fix/workaround (SWELL bug?): EN_CHANGE is not always sent.
		ScheduledJob::Schedule(new OSXForceTxtChangeJob());
#endif
		return -1;
	}
	else if (_msg->message == WM_KEYDOWN || _msg->message == WM_CHAR)
	{
		// Ctrl+A selects all text.
    if (_msg->wParam == 'A' && _iKeyState == LVKF_CONTROL)
		{
			SetFocus(m_edit);
			SendMessage(m_edit, EM_SETSEL, 0, -1);
			return 1;
		}
	}
	return 0; 
}

void NotesWnd::OnTimer(WPARAM wParam)
{
	if (wParam == UPDATE_TIMER)
	{
		// register to marker and region updates only when needed (less stress for REAPER)
		if (g_notesType == SNM_NOTES_RGN_SUB)
			RegisterToMarkerRegionUpdates(&m_mkrRgnListener); // no-op if alreday registered
		else
			UnregisterToMarkerRegionUpdates(&m_mkrRgnListener);

		// no update when editing text or when the view is hidden (e.g. inactive docker tab).
    // when the view is active: update only for regions and if the view is locked
		// => updates during playback, in other cases (e.g. item selection change) the main 
		// window will be the active one, not our NotesWnd
		if (g_notesType!=SNM_NOTES_PROJECT && g_notesType!=SNM_NOTES_PROJECT_EXTRA && g_notesType!=SNM_NOTES_GLOBAL && 
		    IsWindowVisible(m_hwnd) && (!IsActive() || (g_locked && g_notesType == SNM_NOTES_RGN_SUB)))
		{
			Update();
		}
	}
}

void NotesWnd::OnResize()
{
	if (g_notesType != g_prevNotesType)
	{
		// room for buttons?
		if (g_notesType== SNM_NOTES_RGN_SUB) {
			m_resize.get_item(IDC_EDIT1)->orig.bottom = m_resize.get_item(IDC_EDIT1)->real_orig.bottom - 41; //JFB!! 41 is tied to the current .rc!
			m_resize.get_item(IDC_EDIT2)->orig.bottom = m_resize.get_item(IDC_EDIT2)->real_orig.bottom - 41; //JFB!! 41 is tied to the current .rc!
		}
		else {
			m_resize.get_item(IDC_EDIT1)->orig = m_resize.get_item(IDC_EDIT1)->real_orig;
			m_resize.get_item(IDC_EDIT2)->orig = m_resize.get_item(IDC_EDIT2)->real_orig;
		}
		InvalidateRect(m_hwnd, NULL, 0);
	}

  if (g_notesType == SNM_NOTES_RGN_SUB) {
    RECT r;
    GetClientRect(m_hwnd, &r);
    HWND listHwnd = m_actorListView->GetHWND();
    if (!g_hideActorList) {
      int panelWidth = max(150, (int) r.right / 6);
      m_resize.get_item(IDC_EDIT1)->orig.right = m_resize.get_item(IDC_EDIT1)->real_orig.right -
          panelWidth;
      m_resize.get_item(IDC_EDIT1)->orig.bottom = m_resize.get_item(IDC_EDIT1)->real_orig.bottom - 41;
      m_resize.get_item(IDC_EDIT2)->orig.right = m_resize.get_item(IDC_EDIT2)->real_orig.right -
          panelWidth;
      m_resize.get_item(IDC_EDIT2)->orig.bottom = m_resize.get_item(IDC_EDIT2)->real_orig.bottom - 41;
      int listBottom = r.bottom - 16;
      int rightMargin = SNM_GUI_X_MARGIN;
      ShowWindow(listHwnd, SW_SHOWNA);
      SetWindowPos(listHwnd,
                   NULL,
                   r.right - panelWidth - rightMargin,
                   SNM_GUI_TOP_H,
                   panelWidth,
                   listBottom - SNM_GUI_TOP_H,
                   SWP_NOZORDER);
      int vscrollWidth = GetSystemMetrics(SM_CXVSCROLL);
      ListView_SetColumnWidth(listHwnd, 0, 26);
      ListView_SetColumnWidth(listHwnd, 1, panelWidth - 26 - vscrollWidth);
    } else {
      ShowWindow(listHwnd, SW_HIDE);
      m_resize.get_item(IDC_EDIT1)->orig.right = m_resize.get_item(IDC_EDIT1)->real_orig.right;
      m_resize.get_item(IDC_EDIT1)->orig.bottom = m_resize.get_item(IDC_EDIT1)->real_orig.bottom - 41;
      m_resize.get_item(IDC_EDIT2)->orig.right = m_resize.get_item(IDC_EDIT2)->real_orig.right;
      m_resize.get_item(IDC_EDIT2)->orig.bottom = m_resize.get_item(IDC_EDIT2)->real_orig.bottom - 41;
    }
    m_actorListView->Update();
  } else {
    ShowWindow(m_actorListView->GetHWND(), SW_HIDE);
    m_resize.get_item(IDC_EDIT1)->orig.right = m_resize.get_item(IDC_EDIT1)->real_orig.right;
    m_resize.get_item(IDC_EDIT2)->orig.right = m_resize.get_item(IDC_EDIT2)->real_orig.right;
  }
}

void NotesWnd::DrawControls(LICE_IBitmap* _bm, const RECT* _r, int* _tooltipHeight)
{
	int h=SNM_GUI_TOP_H;
	if (_tooltipHeight)
		*_tooltipHeight = h;

	// "big" notes (dynamic font size)
	// drawn first so that it is displayed even with tiny sizing..
	if (g_locked)
	{
		// work on a copy rather than g_lastText (will get modified)
		static char s_drawBuf[sizeof(g_lastText)];
    lstrcpyn(s_drawBuf, g_lastText, sizeof(s_drawBuf));
		if (*s_drawBuf)
		{
			RECT r = *_r;
			r.top += h;
      if (g_notesType == SNM_NOTES_RGN_SUB && !g_hideActorList)
        r.right -= max(150, (int) (_r->right - _r->left) / 6);
			m_bigNotes.SetPosition(&r);

      m_bigNotes.ClearActorColors();
      if (g_notesType == SNM_NOTES_RGN_SUB) {
        int defaultRegionColor = 0;
        if (ColorTheme *ct = SNM_GetColorTheme())
          defaultRegionColor = ct->region;
        const char *hl = g_highlightedActor.Get()->Get();
        bool hasHl = (hl && *hl);
        for (int i = 0; i < g_actors.Get()->GetSize(); i++) {
          SNM_Actor *actor = g_actors.Get()->Get(i);
          if (actor) {
            int color = 0;
            if (hasHl) {
              if (IsActorMatchingHighlight(actor, hl))
                color = actor->GetEffectiveColor();
              else
                color = defaultRegionColor;
            } else {
              color = actor->GetEffectiveColor();
            }
            m_bigNotes.AddActorColor(actor->GetName(), color ? color : defaultRegionColor);
          }
        }
      }

      for (char *p = s_drawBuf; p[0] && p[1]; p++) {
        if (p[0] == '\\' && (p[1] == 'N' || p[1] == 'n')) {
          p[0] = '\n';
          memmove(p + 1, p + 2, strlen(p + 2) + 1);
        }
      }

			m_bigNotes.SetText(s_drawBuf);
			m_bigNotes.SetVisible(true);
		}
	}
	// clear last "big notes"
	else
		LICE_FillRect(_bm,0,h,_bm->getWidth(),_bm->getHeight()-h,WDL_STYLE_GetSysColor(COLOR_WINDOW),0.0,LICE_BLIT_MODE_COPY);


	// 1st row of controls

	IconTheme* it = SNM_GetIconTheme();
	int x0=_r->left+SNM_GUI_X_MARGIN;

	SNM_SkinButton(&m_btnLock, it ? &it->toolbar_lock[!g_locked] : NULL, g_locked ? __LOCALIZE("Разблокировать текст","sws_DLG_152") : __LOCALIZE("Заблокировать текст","sws_DLG_152"));
	if (SNM_AutoVWndPosition(DT_LEFT, &m_btnLock, NULL, _r, &x0, _r->top, h))
	{
		if (SNM_AutoVWndPosition(DT_LEFT, &m_cbType, NULL, _r, &x0, _r->top, h))
		{
      if (!g_locked && g_notesType == SNM_NOTES_RGN_SUB
        && m_overlappingRegionIds.GetSize() > 1) {
        SNM_AutoVWndPosition(DT_LEFT, &m_cbRegion, NULL, _r, &x0, _r->top, h);
        m_cbRegion.SetVisible(true);
						}
						else {
        m_cbRegion.SetVisible(false);
      }

      if (g_notesType == SNM_NOTES_RGN_SUB) {
        SNM_SkinToolbarButton(&m_btnImportSub, __LOCALIZE("Добавить субтитры","sws_DLG_152"));
        if (SNM_AutoVWndPosition(DT_LEFT, &m_btnImportSub, NULL, _r, &x0, _r->top, h)) {
          SNM_SkinToolbarButton(&m_btnClearSubs, __LOCALIZE("Очистить", "sws_DLG_152"));
          if (SNM_AutoVWndPosition(DT_LEFT, &m_btnClearSubs, NULL, _r, &x0, _r->top, h)) {
            const char *videoBtnText = g_videoOverlayEnabled
              ? __LOCALIZE("Видео: ВКЛ", "sws_DLG_152")
              : __LOCALIZE("Видео: ВЫКЛ", "sws_DLG_152");
            SNM_SkinToolbarButton(&m_btnVideoOverlay, videoBtnText);
            if (SNM_AutoVWndPosition(DT_LEFT, &m_btnVideoOverlay, NULL, _r, &x0, _r->top, h)) {
              m_btnVideoOverlay.SetVisible(true);
            } else {
              m_btnVideoOverlay.SetVisible(false);
            }
          } else {
            m_btnVideoOverlay.SetVisible(false);
          }
          m_btnClearSubs.SetVisible(true);
        } else {
          m_btnClearSubs.SetVisible(false);
          m_btnVideoOverlay.SetVisible(false);
        }
        m_btnImportSub.SetVisible(true);
      } else {
        m_btnImportSub.SetVisible(false);
        m_btnClearSubs.SetVisible(false);
        m_btnVideoOverlay.SetVisible(false);
      }

				SNM_AddLogo(_bm, _r, x0, h);
		}
	}

  m_txtLabel.SetVisible(false);

	// 2nd row of controls
  x0 = _r->left+SNM_GUI_X_MARGIN; h=SNM_GUI_BOT_H;
	int y0 = _r->bottom-h;

  if (g_notesType == SNM_NOTES_RGN_SUB && !g_hideActorList)
	{
    int panelWidth = max(150, (int) (_r->right - _r->left) / 6);
    int tableLeft = _r->right - panelWidth - SNM_GUI_X_MARGIN;

    int listTop = SNM_GUI_TOP_H;
    int listBottom = _r->bottom - 16;
    int listRight = tableLeft + panelWidth;
    int pencol = LICE_RGBA_FROMNATIVE(WDL_STYLE_GetSysColor(COLOR_3DSHADOW), 255);
    LICE_Line(_bm,
              tableLeft - 1,
              listTop - 1,
              listRight,
              listTop - 1,
              pencol,
              1.0f,
              LICE_BLIT_MODE_COPY,
              false);
    LICE_Line(_bm,
              tableLeft - 1,
              listBottom,
              listRight,
              listBottom,
              pencol,
              1.0f,
              LICE_BLIT_MODE_COPY,
              false);
    LICE_Line(_bm,
              tableLeft - 1,
              listTop - 1,
              tableLeft - 1,
              listBottom,
              pencol,
              1.0f,
              LICE_BLIT_MODE_COPY,
              false);
    LICE_Line(_bm,
              listRight,
              listTop - 1,
              listRight,
              listBottom,
              pencol,
              1.0f,
              LICE_BLIT_MODE_COPY,
              false);
  }

  if (g_locked)
			return;
	}

bool NotesWnd::GetToolTipString(int _xpos, int _ypos, char* _bufOut, int _bufOutSz)
{
	if (WDL_VWnd* v = m_parentVwnd.VirtWndFromPoint(_xpos,_ypos,1))
	{
		switch (v->GetID())
		{
			case BTNID_LOCK:
        snprintf(_bufOut, _bufOutSz, "%s  (L)", g_locked ? __LOCALIZE("Текст заблокирован", "sws_DLG_152") : __LOCALIZE("Текст разблокирован", "sws_DLG_152"));
        return true;
      case BTNID_IMPORT_SUB:
        snprintf(_bufOut, _bufOutSz, "%s  (Ctrl+I)",
          __LOCALIZE("Добавить субтитры", "sws_DLG_152"));
        return true;
      case BTNID_CLEAR_SUBS:
        snprintf(_bufOut, _bufOutSz, "%s  (Ctrl+Shift+Del)",
          __LOCALIZE("Очистить", "sws_DLG_152"));
				return true;
      case BTNID_VIDEO_OVERLAY:
        snprintf(_bufOut, _bufOutSz, "%s  (V)",
          g_videoOverlayEnabled ? __LOCALIZE("Отключить субтитры на видео", "sws_DLG_152")
                                : __LOCALIZE("Включить субтитры на видео", "sws_DLG_152"));
        return true;
			case CMBID_TYPE:
				lstrcpyn(_bufOut, __LOCALIZE("Режим","sws_DLG_152"), _bufOutSz);
				return true;
			case CMBID_REGION:
				lstrcpyn(_bufOut,
                 __LOCALIZE("Выберите регион для редактирования", "sws_DLG_152"), _bufOutSz);
				return true;
		}
	}
	return false;
}

void NotesWnd::ToggleLock()
{
	g_locked = !g_locked;
	RefreshToolbar(SWSGetCommandID(ToggleNotesLock));
	if (g_notesType == SNM_NOTES_RGN_SUB)
		Update(true); // play vs edit cursor when unlocking
	else
		RefreshGUI();

	if (!g_locked)
		SetFocus(m_edit);
}

void NotesWnd::RefreshActorList() {
  CleanupStaleActors();
  if (m_actorListView)
    m_actorListView->Update();
}

void NotesWnd::ResetSubState() {
  m_overlappingRegionIds.Resize(0);
  m_contextMenuActor = NULL;
  RefreshActorList();
}


///////////////////////////////////////////////////////////////////////////////

void NotesWnd::SaveCurrentText(int _type, bool _wantUndo) 
{
	switch(_type) 
	{
		case SNM_NOTES_PROJECT:
			SaveCurrentProjectNotes(_wantUndo);
			break;
		case SNM_NOTES_PROJECT_EXTRA:
			SaveCurrentExtraProjectNotes(_wantUndo);
			break;
		case SNM_NOTES_ITEM: 
			SaveCurrentItemNotes(_wantUndo); 
			break;
		case SNM_NOTES_TRACK:
			SaveCurrentTrackNotes(_wantUndo);
			break;
		case SNM_NOTES_GLOBAL:
			SaveCurrentGlobalNotes(_wantUndo);
			break;
		case SNM_NOTES_RGN_SUB:
      SaveCurrentRgnSub(_wantUndo);
			break;
	}
}

void NotesWnd::SaveCurrentProjectNotes(bool _wantUndo)
{
	GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
	GetSetProjectNotes(NULL, true, g_lastText, sizeof(g_lastText));
/* project notes are out of the undo system's scope, MarkProjectDirty is the best thing we can do...
	if (_wantUndo)
          Undo_OnStateChangeEx2(NULL, __LOCALIZE("Редактирование заметок проекта","sws_undo"), UNDO_STATE_ALL, -1);
	else
*/
	if (MarkProjectDirty)
		MarkProjectDirty(NULL);
}

void NotesWnd::SaveCurrentExtraProjectNotes(bool _wantUndo)
{
	GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
	g_prjNotes.Get()->Set(g_lastText); // CRLF removed only when saving the project..
		MarkProjectDirty(NULL);
}

void NotesWnd::SaveCurrentItemNotes(bool _wantUndo)
{
	if (g_mediaItemNote && GetMediaItem_Track(g_mediaItemNote))
	{
		GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
		if (GetSetMediaItemInfo(g_mediaItemNote, "P_NOTES", g_lastText))
		{
//				UpdateItemInProject(g_mediaItemNote);
			UpdateTimeline(); // for the item's note button 
				MarkProjectDirty(NULL);
		}
	}
}

void NotesWnd::SaveCurrentTrackNotes(bool _wantUndo)
{
	if (g_trNote && CSurf_TrackToID(g_trNote, false) >= 0)
	{
		GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
		if (SNM_TrackNotes *notes = SNM_TrackNotes::find(g_trNote))
			notes->SetNotes(g_lastText); // CRLF removed only when saving the project
		else
			g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(nullptr, TrackToGuid(g_trNote), g_lastText));

			MarkProjectDirty(NULL);
	}
}

void NotesWnd::SaveCurrentGlobalNotes(bool _wantUndo)
{
	GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
	g_globalNotes.Set(g_lastText);
	/* dirty / modified status is displayed directly in notes window
	if (_wantUndo)
      Undo_OnStateChangeEx2(NULL, __LOCALIZE("Редактирование глобальных заметок", "sws_undo"), UNDO_STATE_MISCCFG, -1);
	else
		MarkProjectDirty(NULL);
	*/

	g_globalNotesDirty = true;
	RefreshGUI(); // to display [modified] in notes window immediately
}

void NotesWnd::SaveCurrentRgnSub(bool _wantUndo)
{
  if (g_hideRegions) {
    if (g_lastMarkerRegionId >= 0) {
      GetWindowText(m_edit, g_lastText, sizeof(g_lastText));
      WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
      SNM_RegionSubtitle *sub = (subs && g_lastMarkerRegionId >= 0 && g_lastMarkerRegionId < subs->GetSize()) ? subs->Get(g_lastMarkerRegionId) : NULL;
      if (sub) {
        sub->SetNotes(g_lastText);
        if (g_videoOverlayEnabled) SyncSubtitlesToVideoOverlay();
      }
      MarkProjectDirty(NULL);
    }
    return;
  }

  if (g_lastMarkerRegionId > 0)
	{
		GetWindowText(m_edit, g_lastText, sizeof(g_lastText));

    SNM_RegionSubtitle *curSub = NULL;
    for (int i=0; i < g_pRegionSubs.Get()->GetSize(); i++) 
    {
      if (g_pRegionSubs.Get()->Get(i)->GetId() == g_lastMarkerRegionId) {
        curSub = g_pRegionSubs.Get()->Get(i);
        curSub->SetNotes(g_lastText);
        break;
      }
    }
    if (!curSub) {
      curSub = new SNM_RegionSubtitle(nullptr, g_lastMarkerRegionId, g_lastText);
      curSub->SetActor("");
      double p1 = 0.0, p2 = 0.0;
      if (EnumMarkerRegionById(NULL, g_lastMarkerRegionId, NULL, &p1, &p2, NULL, NULL, NULL) >= 0)
        curSub->SetTimes(p1, p2);
      g_pRegionSubs.Get()->Add(curSub);
    }
    double pos, end;
    int num, color;
    bool isRgn;
    const char *existingName = NULL;
    if (EnumMarkerRegionById(NULL, g_lastMarkerRegionId, &isRgn, &pos, &end, &existingName, &num, &color) >= 0) {
      WDL_String regionName;
      if (curSub && curSub->GetActor() && curSub->GetActor()[0] && FindActor(curSub->GetActor()))
        BuildRegionName(&regionName, curSub->GetActor(), g_lastText);
      else
        regionName.Set(g_lastText);

      // Track the region-name update to avoid a reentrant notification
      // via NotesMarkerRegionListener::NotifyMarkerRegionUpdate().
      g_internalMkrRgnChange = true;
      SetProjectMarker4(NULL,
                        num,
                        isRgn,
                        pos,
                        end,
                        regionName.Get(),
                        color,
                        !*regionName.Get() ? 1 : 0);
      UpdateTimeline();
    }

    if (g_videoOverlayEnabled) SyncSubtitlesToVideoOverlay();
    MarkProjectDirty(NULL);
  }
	}

///////////////////////////////////////////////////////////////////////////////

void NotesWnd::Update(bool _force)
{
	static bool sRecurseCheck = false;
	if (sRecurseCheck)
		return;

	sRecurseCheck = true;
	ReNotes_LogMsg("NotesWnd::Update: force=%d, type=%d", _force ? 1 : 0, g_notesType);

	// force refresh if needed
	if (_force || g_notesType != g_prevNotesType)
	{
		g_prevNotesType = g_notesType;
		g_mediaItemNote = NULL;
		g_trNote = NULL;
		g_lastMarkerPos = -1.0;
		g_lastMarkerRegionId = -1;
		_force = true; // trick for RefreshGUI() below..
	}

	// update
	int refreshType = NO_REFRESH;
	switch(g_notesType)
	{
		case SNM_NOTES_PROJECT:
		{
			static char s_projNotesBuf[MAX_HELP_LENGTH];
			GetSetProjectNotes(NULL, false, s_projNotesBuf, sizeof(s_projNotesBuf));
			SetText(s_projNotesBuf);
			refreshType = REQUEST_REFRESH;
			break;
		}
		case SNM_NOTES_PROJECT_EXTRA:
			SetText(g_prjNotes.Get()->Get());
			refreshType = REQUEST_REFRESH;
			break;
		case SNM_NOTES_ITEM:
			refreshType = UpdateItemNotes();
			break;
		case SNM_NOTES_TRACK:
			refreshType = UpdateTrackNotes();
			break;
		case SNM_NOTES_GLOBAL:
			SetText(g_globalNotes.Get());
			refreshType = REQUEST_REFRESH;
			break;
		case SNM_NOTES_RGN_SUB:
			refreshType = UpdateRgnSub();
			break;
	}
	
	if (_force || refreshType == REQUEST_REFRESH)
		RefreshGUI();

  if (g_notesType == SNM_NOTES_RGN_SUB) {
    static WDL_PtrList_DOD<SNM_Actor> *s_lastActorPtr = nullptr;
    static int s_lastActorCount = -1;
    WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
    if (_force || actors != s_lastActorPtr || actors->GetSize() != s_lastActorCount) {
      s_lastActorPtr = actors;
      s_lastActorCount = actors->GetSize();
      RefreshActorList();
    }
  }

	sRecurseCheck = false;
}

int NotesWnd::UpdateItemNotes()
{
	int refreshType = NO_REFRESH;
	if (MediaItem* selItem = GetSelectedMediaItem(NULL, 0))
	{
		if (selItem != g_mediaItemNote)
		{
			g_mediaItemNote = selItem;
			if (char* notes = (char*)GetSetMediaItemInfo(g_mediaItemNote, "P_NOTES", NULL))
				SetText(notes, false);
			refreshType = REQUEST_REFRESH;
		} 
	}
	else if (g_mediaItemNote || *g_lastText)
	{
		g_mediaItemNote = NULL;
		SetText("");
		refreshType = REQUEST_REFRESH;
	}
	return refreshType;
}

int NotesWnd::UpdateTrackNotes()
{
	int refreshType = NO_REFRESH;
	if (MediaTrack* selTr = SNM_GetSelectedTrack(NULL, 0, true))
	{
		if (selTr != g_trNote)
		{
			g_trNote = selTr;

			if (SNM_TrackNotes *notes = SNM_TrackNotes::find(g_trNote))
			{
				SetText(notes->GetNotes());
				return REQUEST_REFRESH;
			}

			g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(nullptr, TrackToGuid(g_trNote), ""));
			SetText("");
			refreshType = REQUEST_REFRESH;
		} 
	}
	else if (g_trNote || *g_lastText)
	{
		g_trNote = NULL;
		SetText("");
		refreshType = REQUEST_REFRESH;
	}
	return refreshType;
}

int NotesWnd::UpdateRgnSub()
{
	int refreshType = NO_REFRESH;

	double dPos=GetCursorPositionEx(NULL), accuracy=SNM_FUDGE_FACTOR;
	if (g_locked && GetPlayStateEx(NULL)) { // playing and update required
		dPos = GetPlayPositionEx(NULL);
		accuracy = 0.1; // do not stress REAPER
	}

	if (fabs(g_lastMarkerPos-dPos) > accuracy)
	{
		g_lastMarkerPos = dPos;

    if (g_hideRegions) {
      WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
      WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();

      WDL_TypedBuf<int> newOverlapping;
      for (int i = 0; i < subs->GetSize(); i++) {
        SNM_RegionSubtitle *sub = subs->Get(i);
		if (dPos < sub->GetStartTime() || dPos > sub->GetEndTime())
          continue;
        bool enabled = false;
        for (int j = 0; j < actors->GetSize(); j++) {
          if (!strcmp(actors->Get(j)->GetName(), sub->GetActor())) {
            enabled = actors->Get(j)->IsEnabled();
            break;
				}
						}
        if (enabled)
          newOverlapping.Add(i);
      }

      if (newOverlapping.GetSize() > 0) {
        bool regionsChanged = false;
        if (newOverlapping.GetSize() != m_overlappingRegionIds.GetSize())
          regionsChanged = true;
        else {
          for (int i = 0; i < newOverlapping.GetSize(); i++) {
            if (newOverlapping.Get()[i] != m_overlappingRegionIds.Get()[i]) {
              regionsChanged = true;
              break;
            }
          }
        }

        if (regionsChanged) {
          m_overlappingRegionIds.Resize(newOverlapping.GetSize());
          memcpy(m_overlappingRegionIds.Get(),
                 newOverlapping.Get(),
                 newOverlapping.GetSize() * sizeof(int));

          m_cbRegion.Empty();
          for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
            int subIdx = m_overlappingRegionIds.Get()[i];
            SNM_RegionSubtitle *sub = (subs && subIdx >= 0 && subIdx < subs->GetSize()) ? subs->Get(subIdx) : NULL;
            char itemText[256];
            const char *actor = sub ? sub->GetActor() : NULL;
            if (actor && *actor)
              snprintf(itemText, sizeof(itemText), "%s", actor);
            else
              snprintf(itemText, sizeof(itemText), "Sub %d", i + 1);
            m_cbRegion.AddItem(itemText);
          }

          int selIdx = 0;
          for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
            if (m_overlappingRegionIds.Get()[i] == g_lastMarkerRegionId) {
              selIdx = i;
              break;
            }
          }
          m_cbRegion.SetCurSel2(selIdx);
          g_lastMarkerRegionId = m_overlappingRegionIds.Get()[selIdx];
        }

        if (g_lastMarkerRegionId < 0 && m_overlappingRegionIds.GetSize() > 0) {
          int selIdx = m_cbRegion.GetCurSel2();
          if (selIdx < 0 || selIdx >= m_overlappingRegionIds.GetSize()) selIdx = 0;
          g_lastMarkerRegionId = m_overlappingRegionIds.Get()[selIdx];
        }

        if (g_locked) {
          WDL_FastString displayText;
          bool first = true;
          for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
            int subIdx = m_overlappingRegionIds.Get()[i];
            SNM_RegionSubtitle *sub = (subs && subIdx >= 0 && subIdx < subs->GetSize()) ? subs->Get(subIdx) : NULL;
            if (!sub) continue;
            if (!first)
              displayText.Append("\r\n");
            const char *actor = sub->GetActor();
            bool hasActor = actor && *actor && strcmp(actor, "?") != 0;
            if (hasActor)
              AppendDisplayLine(&displayText, actor, sub->GetNotes());
            else
              displayText.Append(sub->GetNotes());
            first = false;
          }
          SetText(displayText.Get());
        } else if (g_lastMarkerRegionId >= 0) {
          SNM_RegionSubtitle *sub = (subs && g_lastMarkerRegionId >= 0 && g_lastMarkerRegionId < subs->GetSize()) ? subs->Get(g_lastMarkerRegionId) : NULL;
          if (sub)
            SetText(sub->GetNotes());
				}
				refreshType = REQUEST_REFRESH;
      } else if (g_lastMarkerRegionId >= 0 || *g_lastText) {
			g_lastMarkerPos = -1.0;
			g_lastMarkerRegionId = -1;
        m_overlappingRegionIds.Resize(0);
        m_cbRegion.Empty();
			SetText("");
			refreshType = REQUEST_REFRESH;
		}
    } else {
      int id, idx = FindMarkerRegion(NULL, dPos, SNM_REGION_MASK, &id);
		if (id > 0)
		{
        WDL_TypedBuf<int> newOverlappingIds;
        int enumIdx = 0;
        bool isRgn;
        double p1, p2;
        int num;
        while ((enumIdx = EnumProjectMarkers2(NULL, enumIdx, &isRgn, &p1, &p2, NULL, &num))) {
          if (isRgn && dPos >= p1 && dPos <= p2) {
            int regionId = MakeMarkerRegionId(num, true);
            newOverlappingIds.Add(regionId);
          }
        }

        bool regionsChanged = false;
        if (newOverlappingIds.GetSize() != m_overlappingRegionIds.GetSize())
          regionsChanged = true;
        else {
          for (int i = 0; i < newOverlappingIds.GetSize(); i++) {
            if (newOverlappingIds.Get()[i] != m_overlappingRegionIds.Get()[i]) {
              regionsChanged = true;
              break;
            }
          }
        }

        if (regionsChanged) {
          SyncTimelineRegionsToSubtitles();
          m_overlappingRegionIds.Resize(newOverlappingIds.GetSize());
          memcpy(m_overlappingRegionIds.Get(),
                 newOverlappingIds.Get(),
                 newOverlappingIds.GetSize() * sizeof(int));

          m_cbRegion.Empty();
          for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
            int regId = m_overlappingRegionIds.Get()[i];
            SNM_RegionSubtitle *sub = NULL;
            for (int j = 0; j < g_pRegionSubs.Get()->GetSize(); j++) {
              if (g_pRegionSubs.Get()->Get(j)->GetId() == regId) {
                sub = g_pRegionSubs.Get()->Get(j);
                break;
              }
            }
            char itemText[256];
            if (sub && sub->GetActor() && sub->GetActor()[0] && strcmp(sub->GetActor(), "?") != 0) {
              snprintf(itemText, sizeof(itemText), "%s", sub->GetActor());
            } else if (sub && sub->GetNotes() && sub->GetNotes()[0]) {
              snprintf(itemText, sizeof(itemText), "%.30s", sub->GetNotes());
            } else {
              double pos, end;
              const char *rName = NULL;
              int num;
              if (EnumMarkerRegionById(NULL, regId, NULL, &pos, &end, &rName, &num, NULL) >= 0) {
                if (rName && *rName)
                  snprintf(itemText, sizeof(itemText), "%s", rName);
                else
                  snprintf(itemText, sizeof(itemText), "R%d", num);
              } else {
                snprintf(itemText, sizeof(itemText), "Region %d", i + 1);
              }
            }
            m_cbRegion.AddItem(itemText);
          }

          int selIdx = 0;
          for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
            if (m_overlappingRegionIds.Get()[i] == g_lastMarkerRegionId)
			{
              selIdx = i;
              break;
            }
          }
          m_cbRegion.SetCurSel2(selIdx);

          if (m_overlappingRegionIds.GetSize() > 0)
            g_lastMarkerRegionId = m_overlappingRegionIds.Get()[selIdx];
        }

        if (g_lastMarkerRegionId <= 0 && m_overlappingRegionIds.GetSize() > 0) {
          int selIdx = m_cbRegion.GetCurSel2();
          if (selIdx < 0 || selIdx >= m_overlappingRegionIds.GetSize()) selIdx = 0;
          g_lastMarkerRegionId = m_overlappingRegionIds.Get()[selIdx];
        }

        if (g_locked && m_overlappingRegionIds.GetSize() > 0) {
          WDL_FastString displayText;
          bool first = true;
          for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
            int regId = m_overlappingRegionIds.Get()[i];
            SNM_RegionSubtitle *sub = NULL;
            for (int j = 0; j < g_pRegionSubs.Get()->GetSize(); j++) {
              if (g_pRegionSubs.Get()->Get(j)->GetId() == regId) {
                sub = g_pRegionSubs.Get()->Get(j);
                break;
              }
            }
            if (!sub) continue;
            if (!first)
              displayText.Append("\r\n");
            const char *actor = sub->GetActor();
            bool hasActor = actor && *actor && strcmp(actor, "?") != 0;
            if (hasActor)
              AppendDisplayLine(&displayText, actor, sub->GetNotes());
            else
              displayText.Append(sub->GetNotes());
            first = false;
          }
          SetText(displayText.Get());
        } else if (g_lastMarkerRegionId > 0) {
          bool found = false;
          for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
            SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(i);
            if (sub && sub->GetId() == g_lastMarkerRegionId) {
              SetText(sub->GetNotes());
              found = true;
              break;
            }
          }
          if (!found) {
            const char *rName = NULL;
            if (EnumMarkerRegionById(NULL, g_lastMarkerRegionId, NULL, NULL, NULL, &rName, NULL, NULL) >= 0 && rName)
              SetText(rName);
            else
              SetText("");
          }
        } else {
          SetText("");
        }
        refreshType = REQUEST_REFRESH;
      } else if (g_lastMarkerRegionId > 0 || *g_lastText) {
        g_lastMarkerPos = -1.0;
        g_lastMarkerRegionId = -1;
        m_overlappingRegionIds.Resize(0);
        m_cbRegion.Empty();
					SetText("");
        refreshType = REQUEST_REFRESH;
      }
    }
	}
	return refreshType;
}

void NotesWnd::ForceUpdateRgnSub() {
  if (g_locked && m_overlappingRegionIds.GetSize() > 0) {
    WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
    WDL_FastString displayText;
    bool first = true;
    for (int i = 0; i < m_overlappingRegionIds.GetSize(); i++) {
      SNM_RegionSubtitle *sub = NULL;
      if (g_hideRegions) {
        sub = subs->Get(m_overlappingRegionIds.Get()[i]);
      } else {
        int regId = m_overlappingRegionIds.Get()[i];
        for (int j = 0; j < subs->GetSize(); j++) {
          if (subs->Get(j)->GetId() == regId) {
            sub = subs->Get(j);
            break;
          }
        }
      }
      if (!sub) continue;
      if (!first)
        displayText.Append("\r\n");
      const char *actor = sub->GetActor();
      bool hasActor = actor && *actor && strcmp(actor, "?") != 0;
      if (hasActor)
        AppendDisplayLine(&displayText, actor, sub->GetNotes());
      else
        displayText.Append(sub->GetNotes());
      first = false;
    }
    SetText(displayText.Get());
    RefreshGUI();
  } else if (g_hideRegions ? g_lastMarkerRegionId >= 0 : g_lastMarkerRegionId > 0) {
    if (g_hideRegions) {
      SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(g_lastMarkerRegionId);
      if (sub) {
        SetText(sub->GetNotes());
				if (g_locked)
					RefreshGUI();
			}
    } else {
					for (int i=0; i < g_pRegionSubs.Get()->GetSize(); i++) {
        SNM_RegionSubtitle *sub = g_pRegionSubs.Get()->Get(i);
        if (sub->GetId() == g_lastMarkerRegionId) {
							SetText(sub->GetNotes());
				if (g_locked)
					RefreshGUI();
						break;
			}
	}
}
	}
	}


///////////////////////////////////////////////////////////////////////////////
// Encode/decode notes (to/from RPP format)
// WDL's cfg_encode_textblock() & cfg_decode_textblock() would not help here..
///////////////////////////////////////////////////////////////////////////////

bool GetStringFromNotesChunk(WDL_FastString* _notesIn, char* _bufOut, int _bufOutSz)
{
	if (!_bufOut || !_notesIn)
		return false;

	memset(_bufOut, 0, _bufOutSz);
	const char* pNotes = _notesIn->Get();
	if (pNotes && *pNotes)
	{
		// find 1st '|'
		int i=0; while (pNotes[i] && pNotes[i] != '|') i++;
		if (pNotes[i]) i++;
		else return true;
	
		int j=0;
		while (pNotes[i] && j < _bufOutSz)
		{
			if (pNotes[i] != '\r' && pNotes[i] != '\n')
			{
        _bufOut[j++] = (pNotes[i]=='|'&&pNotes[i-1]=='\n' ? '\n' : pNotes[i]); // i is >0 here
			}
			i++;
		}
		if (j>=1 && !strcmp(_bufOut+j-1, ">")) // remove trailing ">", if any
			_bufOut[j-1] = '\0';
	}
	return true;
}

bool GetNotesChunkFromString(const char* _bufIn, WDL_FastString* _notesOut, const char* _startLine)
{
	if (_notesOut && _bufIn)
	{
		if (!_startLine) _notesOut->Set("<NOTES\n|");
		else _notesOut->Set(_startLine);

		int i=0;
		while (_bufIn[i])
		{
			if (_bufIn[i] == '\n') 
				_notesOut->Append("\n|");
			else if (_bufIn[i] != '\r') 
				_notesOut->Append(_bufIn+i, 1);
			i++;
		}
		_notesOut->Append("\n>\n");
		return true;
	}
	return false;
}


///////////////////////////////////////////////////////////////////////////////
// NotesUpdateJob
///////////////////////////////////////////////////////////////////////////////

void NotesUpdateJob::Perform() {
  ReNotes_LogMsg("NotesUpdateJob::Perform: pendingRecreation=%d", g_pendingRegionRecreation ? 1 : 0);
  if (g_pendingRegionRecreation) {
    g_pendingRegionRecreation = false;
    if (!g_hideRegions)
      ShowAllRegions();
  }
  SyncTimelineRegionsToSubtitles();
  if (NotesWnd* w = g_notesWndMgr.Get())
    w->Update(true);
  if (g_videoOverlayEnabled)
    SyncSubtitlesToVideoOverlay();
}


///////////////////////////////////////////////////////////////////////////////
// NotesMarkerRegionListener
///////////////////////////////////////////////////////////////////////////////

// ScheduledJob because of multi-notifs during project switches (vs CSurfSetTrackListChange)
void NotesMarkerRegionListener::NotifyMarkerRegionUpdate(int _updateFlags)
{
  ReNotes_LogMsg("NotifyMarkerRegionUpdate: flags=%d, internalDepth=%d, internalChange=%d",
                 _updateFlags, s_internalMkrRgnChangeDepth, g_internalMkrRgnChange ? 1 : 0);
	if (g_notesType == SNM_NOTES_RGN_SUB)
	{
		if (s_internalMkrRgnChangeDepth > 0 || g_internalMkrRgnChange)
		{
			g_internalMkrRgnChange = false;
			return;
		}
		ScheduledJob::Schedule(new NotesUpdateJob(SNM_SCHEDJOB_ASYNC_DELAY_OPT));
	}
}


///////////////////////////////////////////////////////////////////////////////

void StripAssFormattingTags(WDL_FastString *text) {
  WDL_FastString result;
  const char *p = text->Get();
  while (*p) {
    if (*p == '{' && *(p + 1) == '\\') {
      while (*p && *p != '}')
        p++;
      if (*p == '}')
        p++;
    } else if (*p == '\\' && (*(p + 1) == 'N' || *(p + 1) == 'n')) {
      result.Append("\n");
      p += 2;
    } else {
      result.Append(p, 1);
      p++;
    }
  }
  text->Set(result.Get());
}

void StripSrtFormattingTags(WDL_FastString *text) {
  WDL_FastString result;
  const char *p = text->Get();
  while (*p) {
    if (*p == '<') {
      while (*p && *p != '>')
        p++;
      if (*p == '>')
        p++;
    } else {
      result.Append(p, 1);
      p++;
    }
  }
  text->Set(result.Get());
}

static double OklchSrgbGamma(double x) {
  if (x >= 0.0031308)
    return 1.055 * pow(x, 1.0 / 2.4) - 0.055;
  return 12.92 * x;
}

int GenerateActorColor(const char *actorName) {
  unsigned int hash = 2166136261u;
  const char *p = actorName;
  while (*p) {
    hash ^= (unsigned char) *p++;
    hash *= 16777619u;
  }
  hash ^= hash >> 16;
  hash *= 0x45d9f3bu;
  hash ^= hash >> 16;

  double hDeg = ((hash & 0x3FF) / 1024.0) * 360.0;
  double ch = 0.08 + ((hash >> 10) & 0xFF) / 255.0 * 0.09;
  double lk = 0.45 + ((hash >> 18) & 0xFF) / 255.0 * 0.20;

  double hRad = hDeg * (3.14159265358979323846 / 180.0);
  double a = ch * cos(hRad);
  double b = ch * sin(hRad);

  double l_ = lk + 0.3963377774 * a + 0.2158037573 * b;
  double m_ = lk - 0.1055613458 * a - 0.0638541728 * b;
  double s_ = lk - 0.0894841775 * a - 1.2914855480 * b;

  double l = l_ * l_ * l_;
  double m = m_ * m_ * m_;
  double s = s_ * s_ * s_;

  double rd = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
  double gd = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
  double bd = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

  if (rd < 0.0) rd = 0.0;
  else if (rd > 1.0) rd = 1.0;
  if (gd < 0.0) gd = 0.0;
  else if (gd > 1.0) gd = 1.0;
  if (bd < 0.0) bd = 0.0;
  else if (bd > 1.0) bd = 1.0;

  int ri = (int) (OklchSrgbGamma(rd) * 255.0 + 0.5);
  int gi = (int) (OklchSrgbGamma(gd) * 255.0 + 0.5);
  int bi = (int) (OklchSrgbGamma(bd) * 255.0 + 0.5);

  return RGB(ri, gi, bi) | 0x1000000;
}

int SNM_Actor::GetEffectiveColor() const {
  if (m_hasCustomColor)
    return m_color;
  if (m_linkedActorName.GetLength() > 0)
    return GenerateActorColor(m_linkedActorName.Get());
  return m_color;
}

static SNM_Actor *FindActorInList(WDL_PtrList_DOD<SNM_Actor> *actors, const char *name) {
  for (int i = 0; i < actors->GetSize(); i++)
    if (!strcmp(actors->Get(i)->GetName(), name))
      return actors->Get(i);
  return NULL;
}

static void CopyRoleState(SNM_Actor *target, const SNM_Actor *source) {
  target->SetLinkedActorName(source->GetLinkedActorName());
  target->SetColor(source->GetColor());
  target->SetHasCustomColor(source->HasCustomColor());
}

static void UpdateRoleMemory(const SNM_Actor *actor) {
  WDL_PtrList_DOD<SNM_Actor> *memory = g_importedRoleActors.Get();
  SNM_Actor *saved = FindActorInList(memory, actor->GetName());
  if (!saved) {
    saved = new SNM_Actor(actor->GetName(), actor->GetColor());
    memory->Add(saved);
  }
  CopyRoleState(saved, actor);
}

SNM_Actor *FindOrCreateActor(const char *name) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++)
    if (!strcmp(actors->Get(i)->GetName(), name))
      return actors->Get(i);
  int color = strcmp(name, "?") ? GenerateActorColor(name) : 0;
  SNM_Actor *actor = new SNM_Actor(name, color);
  if (SNM_Actor *saved = FindActorInList(g_importedRoleActors.Get(), name))
    CopyRoleState(actor, saved);
  actors->Add(actor);
  return actor;
}

void CleanupStaleActors() {
  // Roles are kept across imports and episodes even if an episode has no dialogue for them.
  // Unused roles can be explicitly removed via context menu ("Удалить роль" / "Удалить роли без реплик").
}

enum { COL_ENABLED = 0, COL_NAME, COL_COUNT };

static SWS_LVColumn g_actorCols[] = {
  {30, 2, ""},
  {100, 0, "Actor"}
};

ActorListView::ActorListView(HWND hwndList, HWND hwndEdit)
  : SWS_ListView(hwndList,
                 hwndEdit,
                 COL_COUNT,
                 g_actorCols,
                 "ACTORLISTVIEWSTATE",
                 false,
                 "sws_DLG_152") {
  m_iSortCol = COL_NAME + 1;
}

void ActorListView::GetItemList(SWS_ListItemList *pList) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++)
    pList->Add((SWS_ListItem *) actors->Get(i), false);
}

void ActorListView::GetItemText(SWS_ListItem *item, int iCol, char *str, int iStrMax) {
  if (!item || !str || iStrMax <= 0) {
    if (str && iStrMax > 0) str[0] = 0;
    return;
  }

  SNM_Actor *actor = (SNM_Actor *) item;
  if (!actor || g_actors.Get()->Find(actor) < 0) {
    str[0] = 0;
    return;
  }

  switch (iCol) {
    case COL_ENABLED:
#ifdef _WIN32
      lstrcpyn(str, actor->IsEnabled() ? UTF8_CHECKMARK : "", iStrMax);
#else
      lstrcpyn(str, actor->IsEnabled() ? "+" : "-", iStrMax);
#endif
      break;
    case COL_NAME:
#ifdef _WIN32
      lstrcpyn(str, actor->GetName(), iStrMax);
#else
      if (actor->HasLinkedActor()) {
        snprintf(str, iStrMax, "[%s] %s", actor->GetLinkedActorName(), actor->GetName());
      } else {
        lstrcpyn(str, actor->GetName(), iStrMax);
      }
#endif
      break;
    default:
      str[0] = 0;
      break;
  }
}

int ActorListView::OnItemSort(SWS_ListItem *item1, SWS_ListItem *item2) {
  SNM_Actor *actor1 = (SNM_Actor *) item1;
  SNM_Actor *actor2 = (SNM_Actor *) item2;

  if (!actor1 || !actor2 ||
      g_actors.Get()->Find(actor1) < 0 || g_actors.Get()->Find(actor2) < 0)
    return 0;

  int result = _stricmp(actor1->GetName(), actor2->GetName());

  if (m_iSortCol < 0)
    return -result;
  return result;
}

void ActorListView::OnItemClk(SWS_ListItem *item, int iCol, int iKeyState) {
  if (!item)
    return;

  SNM_Actor *clickedActor = (SNM_Actor *) item;
  if (!clickedActor || g_actors.Get()->Find(clickedActor) < 0)
    return;
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();

  if (iKeyState & LVKF_SHIFT) {
    bool clickedEnabled = clickedActor->IsEnabled();

    bool othersEnabled = false;
    for (int i = 0; i < actors->GetSize(); i++) {
      SNM_Actor *a = actors->Get(i);
      if (a != clickedActor && a->IsEnabled()) {
        othersEnabled = true;
        break;
      }
    }

    if (!clickedEnabled) {
      for (int i = 0; i < actors->GetSize(); i++) {
        SNM_Actor *a = actors->Get(i);
        if (a == clickedActor) {
          a->SetEnabled(true);
          RecreateActorRegions(a->GetName());
        } else if (a->IsEnabled()) {
          a->SetEnabled(false);
          DeleteActorRegions(a->GetName());
        }
      }
    } else if (othersEnabled) {
      for (int i = 0; i < actors->GetSize(); i++) {
        SNM_Actor *a = actors->Get(i);
        if (a != clickedActor && a->IsEnabled()) {
          a->SetEnabled(false);
          DeleteActorRegions(a->GetName());
        }
      }
    } else {
      for (int i = 0; i < actors->GetSize(); i++) {
        SNM_Actor *a = actors->Get(i);
        if (a == clickedActor) {
          a->SetEnabled(false);
          DeleteActorRegions(a->GetName());
        } else {
          a->SetEnabled(true);
          RecreateActorRegions(a->GetName());
        }
      }
    }
    UpdateTimeline();
  } else {
    bool newState = !clickedActor->IsEnabled();
    clickedActor->SetEnabled(newState);

    if (newState)
      RecreateActorRegions(clickedActor->GetName());
    else {
      DeleteActorRegions(clickedActor->GetName());
      UpdateTimeline();
    }
  }

  if (g_hideRegions)
    g_lastMarkerPos = -1.0;
  SyncSubtitleItemsTrack(false);
  Update();
  if (g_videoOverlayEnabled)
    SyncSubtitlesToVideoOverlay();
}

bool ActorListView::CustomDrawItem(HDC hdc, SWS_ListItem *item, int iCol, RECT *r, bool selected) {
  if (!item || !hdc || !r)
    return false;

  SNM_Actor *actor = (SNM_Actor *) item;
  if (!actor || g_actors.Get()->Find(actor) < 0)
    return false;

  int sz;
  ColorTheme *ctheme = (ColorTheme *) GetColorThemeStruct(&sz);
  if (!ctheme || sz < (int) sizeof(ColorTheme))
    return false;

  int bgColor = selected
                  ? (GetFocus() == m_hwndList
                       ? ctheme->genlist_sel[0]
                       : ctheme->genlist_selinactive[0])
                  : ctheme->genlist_bg;
  HBRUSH bgBrush = CreateSolidBrush(bgColor);
  FillRect(hdc, r, bgBrush);
  DeleteObject(bgBrush);

  int gridColor = ctheme->genlist_gridlines;
  if (gridColor != bgColor && iCol == COL_NAME) {
    RECT listRect;
    GetClientRect(m_hwndList, &listRect);
    HPEN pen = CreatePen(PS_SOLID, 0, gridColor);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, listRect.left, r->bottom - 1, NULL);
    LineTo(hdc, listRect.right, r->bottom - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
  }

  if (iCol == COL_ENABLED) {
    int textColor = selected
                      ? (GetFocus() == m_hwndList
                           ? ctheme->genlist_sel[1]
                           : ctheme->genlist_selinactive[1])
                      : ctheme->genlist_fg;
    SetTextColor(hdc, textColor);
    SetBkMode(hdc, TRANSPARENT);
    const char *text = actor->IsEnabled() ? UTF8_CHECKMARK : "";
    DrawTextUTF8(hdc, text, -1, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else if (iCol == COL_NAME) {
    int h = r->bottom - r->top;
    int dotSize = 8;
    int dotX = r->left + 6;
    int textLeft = dotX + dotSize + 4;

    const char *hl = g_highlightedActor.Get()->Get();
    bool hasHl = (hl && *hl);
    bool isMatched = hasHl && IsActorMatchingHighlight(actor, hl);

    int dotColor = 0;
    if (hasHl) {
      dotColor = isMatched ? actor->GetEffectiveColor() : 0;
    } else {
      dotColor = actor->GetEffectiveColor();
    }
    HBRUSH dotBrush = CreateSolidBrush((dotColor ? dotColor : ctheme->region) & 0xFFFFFF);

    int textColor = selected
                      ? (GetFocus() == m_hwndList
                           ? ctheme->genlist_sel[1]
                           : ctheme->genlist_selinactive[1])
                      : (hasHl && isMatched
                           ? ((actor->GetEffectiveColor() ? actor->GetEffectiveColor() : ctheme->genlist_fg) & 0xFFFFFF)
                           : ctheme->genlist_fg);
    SetBkMode(hdc, TRANSPARENT);

    if (actor->HasLinkedActor()) {
      int halfH = h / 2;
      int dotY = r->top + (halfH - dotSize) / 2;
      RECT dotRect = {dotX, dotY, dotX + dotSize, dotY + dotSize};
      FillRect(hdc, &dotRect, dotBrush);

      SetTextColor(hdc, textColor);
      RECT topRect = {textLeft, r->top, r->right, r->top + halfH};
      DrawTextUTF8(hdc,
                   actor->GetName(),
                   -1,
                   &topRect,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

      int effColor = actor->GetEffectiveColor();
      int linkedColor = (hasHl && !isMatched) ? 0 : effColor;
      SetTextColor(hdc, (linkedColor ? linkedColor : ctheme->region) & 0xFFFFFF);
      RECT botRect = {textLeft, r->top + halfH, r->right, r->bottom};
      DrawTextUTF8(hdc,
                   actor->GetLinkedActorName(),
                   -1,
                   &botRect,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
      int dotY = r->top + (h - dotSize) / 2;
      RECT dotRect = {dotX, dotY, dotX + dotSize, dotY + dotSize};
      FillRect(hdc, &dotRect, dotBrush);

      SetTextColor(hdc, textColor);
      RECT textRect = {textLeft, r->top, r->right, r->bottom};
      DrawTextUTF8(hdc,
                   actor->GetName(),
                   -1,
                   &textRect,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    DeleteObject(dotBrush);
  }

  return true;
}

bool ActorListView::GetCustomColumnColor(SWS_ListItem *item,
                                         int iCol,
                                         bool selected,
                                         COLORREF *textColor) {
  if (!item || !textColor)
    return false;

  SNM_Actor *actor = (SNM_Actor *) item;
  if (!actor || g_actors.Get()->Find(actor) < 0)
    return false;

  if (iCol == COL_NAME) {
    int sz;
    ColorTheme *ctheme = (ColorTheme *) GetColorThemeStruct(&sz);
    const char *hl = g_highlightedActor.Get()->Get();
    bool hasHl = (hl && *hl);
    bool isMatched = hasHl && IsActorMatchingHighlight(actor, hl);

    int color = 0;
    if (hasHl) {
      color = isMatched ? actor->GetEffectiveColor() : 0;
    } else {
      color = actor->GetEffectiveColor();
    }
    if (color) {
      *textColor = color & 0xFFFFFF;
      return true;
    }
    if (ctheme && sz >= (int) sizeof(ColorTheme)) {
      *textColor = ctheme->region & 0xFFFFFF;
      return true;
    }
  }

  return false;
}

static std::vector<std::string> SplitAssActors(const char *actorStart, int actorLen) {
  std::vector<std::string> actors;
  const std::string field = actorStart && actorLen > 0
    ? std::string(actorStart, actorLen) : std::string();
  size_t start = 0;
  while (start <= field.size()) {
    const size_t separator = field.find(';', start);
    const size_t end = separator == std::string::npos ? field.size() : separator;
    size_t nameStart = start;
    size_t nameEnd = end;
    while (nameStart < nameEnd && (field[nameStart] == ' ' || field[nameStart] == '\t'))
      nameStart++;
    while (nameEnd > nameStart && (field[nameEnd - 1] == ' ' || field[nameEnd - 1] == '\t'))
      nameEnd--;
    if (nameEnd > nameStart) {
      const std::string name = field.substr(nameStart, nameEnd - nameStart);
      if (std::find(actors.begin(), actors.end(), name) == actors.end())
        actors.push_back(name);
    }
    if (separator == std::string::npos)
      break;
    start = separator + 1;
  }
  if (actors.empty())
    actors.push_back("?");
  return actors;
}

static std::string TrimAssDialogueLine(const std::string &line) {
  size_t start = 0;
  size_t end = line.size();
  while (start < end && (line[start] == ' ' || line[start] == '\t' || line[start] == '\r'))
    start++;
  while (end > start
         && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r'))
    end--;
  return line.substr(start, end - start);
}

static bool StartsWithDialogueDash(const std::string &line) {
  return !line.empty() && (line[0] == '-'
    || line.compare(0, 3, "\xE2\x80\x93") == 0
    || line.compare(0, 3, "\xE2\x80\x94") == 0);
}

static std::vector<std::string> SplitAssDialogueText(const char *text,
                                                      size_t actorCount) {
  std::vector<std::string> result(actorCount);
  if (!actorCount)
    return result;
  if (actorCount == 1) {
    result[0] = text ? text : "";
    return result;
  }

  std::vector<std::string> lines;
  const std::string source = text ? text : "";
  size_t start = 0;
  while (start <= source.size()) {
    const size_t separator = source.find('\n', start);
    const size_t end = separator == std::string::npos ? source.size() : separator;
    const std::string line = TrimAssDialogueLine(source.substr(start, end - start));
    if (!line.empty())
      lines.push_back(line);
    if (separator == std::string::npos)
      break;
    start = separator + 1;
  }

  std::vector<std::string> phrases;
  for (const std::string &line : lines) {
    if (phrases.empty() || StartsWithDialogueDash(line))
      phrases.push_back(line);
    else {
      phrases.back().append("\n");
      phrases.back().append(line);
    }
  }

  if (phrases.size() != actorCount && lines.size() == actorCount)
    phrases = lines;

  if (phrases.size() == 1 && actorCount > 1) {
    for (size_t i = 0; i < actorCount; i++)
      result[i] = phrases[0];
    return result;
  }

  const size_t directCount = std::min(actorCount, phrases.size());
  for (size_t i = 0; i < directCount; i++)
    result[i] = phrases[i];
  for (size_t i = actorCount; i < phrases.size(); i++) {
    if (!result.back().empty())
      result.back().append("\n");
    result.back().append(phrases[i]);
  }
  return result;
}

bool ImportAssFile(const char *_fn) {
  bool ok = false;
  double firstPos = -1.0;
  bool inEventsSection = false;

  if (FILE *f = fopenUTF8(_fn, "rt")) {
    char buf[4096];
    if (fgets(buf, 4, f) && strcmp("\xEF\xBB\xBF", buf))
      rewind(f);

    while (fgets(buf, sizeof(buf), f)) {
      if (buf[0] == '[') {
        inEventsSection = (_strnicmp(buf, "[Events]", 8) == 0);
        continue;
      }

      if (!inEventsSection)
        continue;

      if (_strnicmp(buf, "Dialogue:", 9) != 0)
        continue;

      const char *p = buf + 9;
      while (*p == ' ')
        p++;

      int field = 0;
      int p1[3] = {0}, p1cs = 0;
      int p2[3] = {0}, p2cs = 0;
      const char *textStart = NULL;
      const char *actorStart = NULL;
      int actorLen = 0;
      bool validTimes = true;

      while (*p && field < 9) {
        const char *fieldStart = p;
        while (*p && *p != ',')
          p++;

        if (field == 1) {
          if (sscanf(fieldStart, "%d:%d:%d.%d", &p1[0], &p1[1], &p1[2], &p1cs) != 4)
            validTimes = false;
        } else if (field == 2) {
          if (sscanf(fieldStart, "%d:%d:%d.%d", &p2[0], &p2[1], &p2[2], &p2cs) != 4)
            validTimes = false;
        } else if (field == 4) {
          actorStart = fieldStart;
          const char *actorEnd = p;
          actorLen = (int) (actorEnd - actorStart);
        }

        if (*p == ',')
          p++;
        field++;
      }

      if (field == 9)
        textStart = p;

      if (!validTimes)
        continue;

      if (!textStart || !*textStart)
        continue;

      const char *textEnd = textStart;
      while (*textEnd && *textEnd != '\r' && *textEnd != '\n')
        textEnd++;

      WDL_FastString notes;
      notes.Append(textStart, (int) (textEnd - textStart));
      StripAssFormattingTags(&notes);

      double startTime = p1[0] * 3600 + p1[1] * 60 + p1[2] + double(p1cs) / 100;
      double endTime = p2[0] * 3600 + p2[1] * 60 + p2[2] + double(p2cs) / 100;

      const std::vector<std::string> actors = SplitAssActors(actorStart, actorLen);
      const std::vector<std::string> actorNotes =
        SplitAssDialogueText(notes.Get(), actors.size());
      for (size_t actorIndex = 0; actorIndex < actors.size(); actorIndex++) {
        const std::string &actorName = actors[actorIndex];
        const bool hasRealActor = (!actorName.empty() && actorName != "?");
        SNM_Actor *actorObj = hasRealActor ? FindOrCreateActor(actorName.c_str()) : NULL;
        if (actorNotes[actorIndex].empty())
          continue;
        const char *roleNotes = actorNotes[actorIndex].c_str();
        const char *actStr = hasRealActor ? actorName.c_str() : "";
        WDL_String name;
        BuildRegionName(&name, actStr, roleNotes);

        if (g_hideRegions) {
          SNM_RegionSubtitle *sub = new SNM_RegionSubtitle(nullptr, -1, roleNotes);
          sub->SetActor(actStr);
          sub->SetTimes(startTime, endTime);
          g_pRegionSubs.Get()->Add(sub);
          ok = true;
          if (firstPos < 0.0) firstPos = startTime;
        } else {
          int color = (hasRealActor && g_coloredRegions && actorObj) ? actorObj->GetEffectiveColor() : 0;
          int num = AddProjectMarker2(NULL,
                                      true,
                                      startTime,
                                      endTime,
                                      name.Get(),
                                      -1,
                                      color);
          if (num >= 0) {
            ok = true;
            if (firstPos < 0.0)
              firstPos = startTime;
            int id = MakeMarkerRegionId(num, true);
            if (id > 0) {
              SNM_RegionSubtitle *sub = new SNM_RegionSubtitle(nullptr, id, roleNotes);
              sub->SetActor(actStr);
              sub->SetTimes(startTime, endTime);
              g_pRegionSubs.Get()->Add(sub);
            }
          }
        }
      }
    }
    fclose(f);
  }

  if (ok) {
    if (!g_hideRegions)
      UpdateTimeline();
    if (firstPos > 0.0)
      SetEditCurPos2(NULL, firstPos, true, false);
  }
  return ok;
}

// Import subtitles in the SubRip (.srt) format.
// See https://en.wikipedia.org/wiki/SubRip#Specifications.
bool ImportSubRipFile(const char* _fn)
{
	bool ok = false;
	double firstPos = -1.0;

	// no need to check extension here, it's done for us
	if (FILE* f = fopenUTF8(_fn, "rt"))
	{
		char buf[1024];
		if (fgets(buf, 4, f) && strcmp("\xEF\xBB\xBF", buf)) // UTF-8 BOM
			rewind(f);
		while (fgets(buf, sizeof(buf), f) && *buf)
		{
			if (int num = atoi(buf))
			{
				if (fgets(buf, sizeof(buf), f) && *buf)
				{
					int p1[4], p2[4];
					if (sscanf(buf, "%d:%d:%d,%d --> %d:%d:%d,%d", 
						&p1[0], &p1[1], &p1[2], &p1[3], 
						&p2[0], &p2[1], &p2[2], &p2[3]) != 8)
					{
						break;
					}

					WDL_FastString notes;
					while (fgets(buf, sizeof(buf), f) && *buf) {
						if (*buf == '\r' || *buf == '\n') break;
						notes.Append(buf);
					}
          StripSrtFormattingTags(&notes);
          
          SNM_Actor *actorObj = FindOrCreateActor("?");

          WDL_String name;
          BuildRegionName(&name, "?", notes.Get());

          double startTime = p1[0]*3600 + p1[1]*60 + p1[2] + double(p1[3])/1000;
          double endTime = p2[0] * 3600 + p2[1] * 60 + p2[2] + double(p2[3]) / 1000;

          if (g_hideRegions) {
            SNM_RegionSubtitle *sub = new SNM_RegionSubtitle(nullptr, -1, notes.Get(), "?");
            sub->SetTimes(startTime, endTime);
            g_pRegionSubs.Get()->Add(sub);
            ok = true;
            if (firstPos < 0.0) firstPos = startTime;
          } else {
            int color = g_coloredRegions ? actorObj->GetEffectiveColor() : 0;
            num = AddProjectMarker2(NULL,
                                    true,
                                    startTime,
                                    endTime,
                                    name.Get(),
                                    num,
                                    color);

            if (num >= 0) {
              ok = true;

						if (firstPos < 0.0)
                firstPos = startTime;

						int id = MakeMarkerRegionId(num, true);
						if (id > 0) {
                SNM_RegionSubtitle *sub = new SNM_RegionSubtitle(nullptr, id, notes.Get(), "?");
                sub->SetTimes(startTime, endTime);
                g_pRegionSubs.Get()->Add(sub);
					}
				}
          }
				}
				else
					break;
			}
		}
		fclose(f);
	}
	
	if (ok)
	{
    if (!g_hideRegions)
      UpdateTimeline();
		if (firstPos > 0.0)
			SetEditCurPos2(NULL, firstPos, true, false);
	}
	return ok;
}

void ImportSubTitleFile(COMMAND_T* _ct)
{
	if (char* fn = BrowseForFiles(__LOCALIZE("ReNotes - Добавить субтитры","sws_DLG_152"), g_lastImportSubFn, NULL, false, SNM_SUB_EXT_LIST))
	{
		lstrcpyn(g_lastImportSubFn, fn, sizeof(g_lastImportSubFn));
    bool imported = false;
    if (HasFileExtension(fn, "ASS"))
      imported = ImportAssFile(fn);
		else
      imported = ImportSubRipFile(fn);
    if (imported) {
      Undo_OnStateChangeEx2(NULL, __LOCALIZE("ReNotes: Добавить субтитры", "sws_DLG_152"), UNDO_STATE_ALL, -1);
      MarkProjectDirty(NULL);
      if (NotesWnd *w = g_notesWndMgr.Get())
        w->RefreshActorList();
      SyncSubtitleItemsTrack(false);

      // Auto-ensure Video Processor on video track and enable video overlay
      g_videoOverlayEnabled = true;
      WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlay", "1", g_SNM_IniFn.Get());
      MediaTrack *vtr = FindVideoTrack(true);
      if (vtr) {
        int fx = EnsureVideoSubtitleFX(vtr, true);
        if (fx >= 0) ApplyVideoSubtitleFXParams(vtr, fx);
      }
      SyncSubtitlesToVideoOverlay();
      if (NotesWnd *w = g_notesWndMgr.Get())
        w->RefreshGUI();
    } else
			MessageBox(GetMainHwnd(), __LOCALIZE("Некорректный файл субтитров!","sws_DLG_152"), __LOCALIZE("ReNotes - Ошибка","sws_DLG_152"), MB_OK);
		free(fn);
	}
}

void NotesWnd::OnDroppedFiles(HDROP h) {
  if (g_notesType != SNM_NOTES_RGN_SUB) {
    DragFinish(h);
    return;
			}
  int iFiles = DragQueryFile(h, 0xFFFFFFFF, NULL, 0);
  bool imported = false;
  for (int i = 0; i < iFiles; i++) {
    char fn[SNM_MAX_PATH] = "";
    DragQueryFile(h, i, fn, sizeof(fn));
    if (HasFileExtension(fn, "ASS")) {
      if (ImportAssFile(fn)) imported = true;
      lstrcpyn(g_lastImportSubFn, fn, sizeof(g_lastImportSubFn));
    } else if (HasFileExtension(fn, "SRT")) {
      if (ImportSubRipFile(fn)) imported = true;
      lstrcpyn(g_lastImportSubFn, fn, sizeof(g_lastImportSubFn));
    } else if (HasFileExtension(fn, "ROLES")) {
      if (ImportRolesFile(fn)) {
        lstrcpyn(g_lastRolesFn, fn, sizeof(g_lastRolesFn));
        UpdateRegionColors();
        RefreshActorList();
        ForceUpdateRgnSub();
        RefreshGUI();
        SyncSubtitleItemsTrack(false);
        MarkProjectDirty(NULL);
      } else {
        MessageBox(GetMainHwnd(),
                   __LOCALIZE("Некорректный файл ролёвки!", "sws_DLG_152"),
                   __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                   MB_OK);
      }
    } else {
      MessageBox(GetMainHwnd(),
                 __LOCALIZE("Некорректный файл субтитров!", "sws_DLG_152"),
                 __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                 MB_OK);
					}
				}
  if (imported) {
    Undo_OnStateChangeEx2(NULL, __LOCALIZE("ReNotes: Добавить субтитры", "sws_DLG_152"), UNDO_STATE_ALL, -1);
    MarkProjectDirty(NULL);
    RefreshActorList();
    SyncSubtitleItemsTrack(false);

    // Auto-ensure Video Processor on video track and enable video overlay
    g_videoOverlayEnabled = true;
    WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlay", "1", g_SNM_IniFn.Get());
    MediaTrack *vtr = FindVideoTrack(true);
    if (vtr) {
      int fx = EnsureVideoSubtitleFX(vtr, true);
      if (fx >= 0) ApplyVideoSubtitleFXParams(vtr, fx);
    }
    SyncSubtitlesToVideoOverlay();
    RefreshGUI();
  }
  DragFinish(h);
}

static std::string RoleProfileNameFromPath(const char *path) {
  const char *leaf = path ? path : "";
  for (const char *p = leaf; *p; p++)
    if (*p == '/' || *p == '\\')
      leaf = p + 1;
  std::string name(leaf);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos)
    name.erase(dot);
  if (name.empty())
    name = "Без имени";
  for (size_t i = 0; i < name.size(); i++) {
    unsigned char c = (unsigned char)name[i];
    if (c < 32 || strchr("<>:\"/\\|?*", c))
      name[i] = '_';
  }
  return name;
}

static std::string CurrentProjectRoleProfileName() {
  char projectPath[SNM_MAX_PATH] = "";
  EnumProjects(-1, projectPath, sizeof(projectPath));
  return RoleProfileNameFromPath(projectPath);
}

static WDL_FastString RoleProfilesDirectory(bool create) {
  WDL_FastString dir(GetResourcePath());
  dir.Append(WDL_DIRCHAR_STR "ReNotesRoles");
  if (create)
    CreateDirectory(dir.Get(), NULL);
  return dir;
}

static WDL_FastString RoleProfilePath(const char *name, bool createDirectory) {
  WDL_FastString path(RoleProfilesDirectory(createDirectory).Get());
  path.Append(WDL_DIRCHAR_STR);
  path.Append(name);
  path.Append(".roles");
  return path;
}

static void RefreshRoleProfileEntries() {
  g_roleProfileMenuEntries.clear();
  WDL_FastString directory(RoleProfilesDirectory(false).Get());
  WDL_DirScan scan;
#ifdef _WIN32
  WDL_FastString search(directory.Get());
  search.Append(WDL_DIRCHAR_STR "*.roles");
  int result = scan.First(search.Get(), true);
#else
  int result = scan.First(directory.Get());
#endif
  if (result == 0) {
    do {
      std::string fileName(scan.GetCurrentFN());
      if (fileName.size() <= 6 || _stricmp(fileName.c_str() + fileName.size() - 6, ".roles"))
        continue;
      RoleProfileMenuEntry profile;
      profile.name.assign(fileName, 0, fileName.size() - 6);
      WDL_FastString path(directory.Get());
      path.Append(WDL_DIRCHAR_STR);
      path.Append(fileName.c_str());
      profile.path = path.Get();
      g_roleProfileMenuEntries.push_back(profile);
    } while (!scan.Next() && g_roleProfileMenuEntries.size() < ROLE_PROFILE_MSG_COUNT);
  }
  std::sort(g_roleProfileMenuEntries.begin(), g_roleProfileMenuEntries.end(),
            [](const RoleProfileMenuEntry &a, const RoleProfileMenuEntry &b) {
              return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
            });
}

static void MergeCurrentRolesIntoMemory() {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++)
    UpdateRoleMemory(actors->Get(i));
}

static bool SaveActiveRoleProfile() {
  if (!g_activeRoleProfile.Get()->GetLength())
    g_activeRoleProfile.Get()->Set(CurrentProjectRoleProfileName().c_str());
  MergeCurrentRolesIntoMemory();
  if (!g_importedRoleActors.Get()->GetSize() && !g_actors.Get()->GetSize())
    return false;
  WDL_FastString path(RoleProfilePath(g_activeRoleProfile.Get()->Get(), true).Get());
  return ExportRolesFile(path.Get());
}

static bool ImportRoleProfile(const char *name, const char *path) {
  if (!LoadRolesFile(path))
    return false;
  g_activeRoleProfile.Get()->Set(name);
  return true;
}

bool ExportRolesFile(const char*fn) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();

  WDL_PtrList<SNM_Actor> merged;
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *a = actors->Get(i);
    merged.Add(a);
  }
  WDL_PtrList_DOD<SNM_Actor> *imported = g_importedRoleActors.Get();
  for (int i = 0; i < imported->GetSize(); i++) {
    SNM_Actor *ia = imported->Get(i);
    bool found = false;
    for (int j = 0; j < actors->GetSize(); j++) {
      if (!strcmp(actors->Get(j)->GetName(), ia->GetName())) {
        found = true;
        break;
      }
    }
    if (!found)
      merged.Add(ia);
  }

  if (merged.GetSize() == 0)
    return false;

  FILE* f = fopenUTF8(fn, "w");
  if (!f)
    return false;

  WDL_PtrList<const char> writtenLinked;
  for (int i = 0; i < merged.GetSize(); i++) {
    SNM_Actor *a = merged.Get(i);
    if (!a->HasLinkedActor())
      continue;
    const char *linked = a->GetLinkedActorName();
    bool alreadyWritten = false;
    for (int j = 0; j < writtenLinked.GetSize(); j++) {
      if (!strcmp(writtenLinked.Get(j), linked)) {
        alreadyWritten = true;
        break;
      }
    }
    if (alreadyWritten)
      continue;
    writtenLinked.Add(linked);

    fprintf(f, "[%s]\n", linked);
    bool colorWritten = false;
    for (int j = 0; j < merged.GetSize(); j++) {
      SNM_Actor *b = merged.Get(j);
      if (b->HasLinkedActor() && !strcmp(b->GetLinkedActorName(), linked)) {
        if (!colorWritten && b->HasCustomColor()) {
          int c = b->GetColor() & 0xFFFFFF;
          int r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
          fprintf(f, "color = #%02X%02X%02X\n", r, g, bl);
          colorWritten = true;
        }
        fprintf(f, "character = %s\n", b->GetName());
      }
    }
    fprintf(f, "\n");
  }

  for (int i = 0; i < merged.GetSize(); i++) {
    SNM_Actor *a = merged.Get(i);
    if (!a->HasLinkedActor()) {
      fprintf(f, "[character:%s]\n", a->GetName());
      if (a->HasCustomColor()) {
        int c = a->GetColor() & 0xFFFFFF;
        int r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
        fprintf(f, "color = #%02X%02X%02X\n", r, g, bl);
      }
      fprintf(f, "\n");
    }
  }

  fclose(f);
  return true;
}

static bool LoadRolesFile(const char *fn) {
  FILE *f = fopenUTF8(fn, "rt");
  if (!f)
    return false;

  enum { SECTION_NONE, SECTION_LINKED, SECTION_CHARACTER };
  int sectionType = SECTION_NONE;
  WDL_FastString sectionName;
  int parsedColor = 0;
  bool hasColor = false;
  bool stateCleared = false;
  WDL_PtrList_DOD<WDL_FastString> charNames;

  auto applySection = [&]() {
    if (sectionType == SECTION_LINKED)
    {
      for (int i=0; i < charNames.GetSize(); i++) {
        const char *charName = charNames.Get(i)->Get();
        SNM_Actor *ia = new SNM_Actor(charName,
                                      hasColor ? parsedColor : GenerateActorColor(charName));
        ia->SetLinkedActorName(sectionName.Get());
        if (hasColor) ia->SetHasCustomColor(true);
        if (SNM_Actor *old = FindActorInList(g_importedRoleActors.Get(), charName))
          g_importedRoleActors.Get()->Delete(g_importedRoleActors.Get()->Find(old), true);
        g_importedRoleActors.Get()->Add(ia);
        SNM_Actor *a = FindActorInList(g_actors.Get(), charName);
        if (!a) {
          a = new SNM_Actor(charName, ia->GetColor());
          g_actors.Get()->Add(a);
        }
        CopyRoleState(a, ia);
      }
    } else if (sectionType == SECTION_CHARACTER) {
      SNM_Actor *ia = new SNM_Actor(sectionName.Get(),
                                    hasColor ? parsedColor : GenerateActorColor(sectionName.Get()));
      if (hasColor) ia->SetHasCustomColor(true);
      if (SNM_Actor *old = FindActorInList(g_importedRoleActors.Get(), sectionName.Get()))
        g_importedRoleActors.Get()->Delete(g_importedRoleActors.Get()->Find(old), true);
      g_importedRoleActors.Get()->Add(ia);
      SNM_Actor *a = FindActorInList(g_actors.Get(), sectionName.Get());
      if (!a) {
        a = new SNM_Actor(sectionName.Get(), ia->GetColor());
        g_actors.Get()->Add(a);
      }
      CopyRoleState(a, ia);
    }
  };

  char buf[4096];
  if (fgets(buf, 4, f) && strcmp("\xEF\xBB\xBF", buf))
    rewind(f);

  while (fgets(buf, sizeof(buf), f)) {
    char *line = buf;
    while (*line == ' ' || *line == '\t')
      line++;
    char *end = line + strlen(line);
    while (end > line && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t'))
      end--;
    *end = '\0';

    if (!*line || *line == '#' || *line == ';')
      continue;

    if (*line == '[') {
      if (!stateCleared) {
        g_importedRoleActors.Get()->Empty(true);
        WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
        for (int i = 0; i < actors->GetSize(); i++)
        {
          SNM_Actor *a = actors->Get(i);
          a->SetLinkedActorName("");
          a->SetHasCustomColor(false);
          a->SetColor(GenerateActorColor(a->GetName()));
        }
        stateCleared = true;
      }
      applySection();

      charNames.Empty(true);
      hasColor = false;
      parsedColor = 0;

      char *closing = strrchr(line, ']');
      if (!closing)
        continue;
      *closing = '\0';
      const char *header = line + 1;

      if (_strnicmp(header, "character:", 10) == 0) {
        sectionType = SECTION_CHARACTER;
        sectionName.Set(header + 10);
      } else {
        sectionType = SECTION_LINKED;
        sectionName.Set(header);
      }
      continue;
    }

    if (sectionType == SECTION_NONE)
      continue;

    char *eq = strchr(line, '=');
    if (!eq)
      continue;

    char *keyEnd = eq;
    while (keyEnd > line && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t'))
      keyEnd--;
    *keyEnd = '\0';

    char *value = eq + 1;
    while (*value == ' ' || *value == '\t')
      value++;

    if (_stricmp(line, "color") == 0 && *value == '#' && strlen(value) == 7) {
      int r, g, b;
      if (sscanf(value + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
        parsedColor = RGB(r, g, b) | 0x1000000;
        hasColor = true;
      }
    } else if (_stricmp(line, "character") == 0 && *value) {
      charNames.Add(new WDL_FastString(value));
    }
  }

  if (!stateCleared)
  {
    fclose(f);
    return false;
  }

  applySection();
  fclose(f);
  return true;
}

bool ImportRolesFile(const char *fn) {
  const std::string profileName = RoleProfileNameFromPath(fn);
  if (!ImportRoleProfile(profileName.c_str(), fn))
    return false;
  SaveActiveRoleProfile();
  return true;
}

void ClearAllSubtitles()
{
  Undo_BeginBlock2(NULL);
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  for (int i = subs->GetSize() - 1; i >= 0; i--) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (sub && sub->IsValid()) {
      int idx = GetMarkerRegionIndexFromId(NULL, sub->GetId());
      if (idx >= 0)
        DeleteProjectMarkerByIndex(NULL, idx);
    }
  }
  subs->Empty(true);

  g_highlightedActor.Get()->Set("");

  UpdateTimeline();
  SyncSubtitlesToVideoOverlay();
  MarkProjectDirty(NULL);
  Undo_EndBlock2(NULL, __LOCALIZE("ReNotes: Удалить все субтитры", "sws_DLG_152"), UNDO_STATE_ALL);
}

void WriteGlobalNotesToFile()
{
	if (!g_globalNotesDirty) return;
	
	// Write global notes to <REAPER resource path>/SWS_Global ReNotes.txt.
	WDL_FastString filePath;
	filePath.SetFormatted(SNM_MAX_PATH, "%s/SWS_Global ReNotes.txt", GetResourcePath());
	WDL_FileWrite outfile(filePath.Get(), 1, 65536, 16, 16); // NF: not sure about these params, taken from
	//https://github.com/justinfrankel/licecap/blob/3721ce33ac72ff05ef89d2e92ca58a0f96164134/WDL/lice/lice_gif_write.cpp#L386
	const int globalNotesLength = g_globalNotes.GetLength();
	const int wrtPos = outfile.Write(g_globalNotes.Get(), globalNotesLength);
	if (wrtPos == globalNotesLength) // writing OK
	{
		g_globalNotesDirty = false;
		if (NotesWnd* w = g_notesWndMgr.Get())
			w->RefreshGUI();
	}
	else // writing failed
	{
		MessageBox(GetMainHwnd(), __LOCALIZE("Не удалось сохранить глобальные заметки.", "sws_mbox"), __LOCALIZE("ReNotes - Ошибка", "sws_mbox"), MB_OK);
	}
}


///////////////////////////////////////////////////////////////////////////////
// project_config_extension_t
///////////////////////////////////////////////////////////////////////////////

static bool ReadTrimmedLine(ProjectStateContext *ctx, char *buf, int bufSize) {
  if (!ctx || !buf || bufSize <= 0) return false;
  char line[SNM_MAX_CHUNK_LINE_LENGTH] = "";
  if (ctx->GetLine(line, sizeof(line)))
    return false;
  char *p = line;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  int len = (int)strlen(p);
  while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
    p[--len] = '\0';
  }
  lstrcpyn(buf, p, bufSize);
  return true;
}

static bool ProcessExtensionLine(const char *line, ProjectStateContext *ctx, bool isUndo, struct project_config_extension_t *reg)
{
	if (isUndo)
		return false;

	LineParser lp(false);
	if (lp.parse(line) || lp.getnumtokens() < 1)
		return false;

	ReaProject *p = GetCurrentProjectInLoadSave();

	if (!strcmp(lp.gettoken_str(0), "<S&M_ROLE_PROFILE"))
	{
		char profileName[SNM_MAX_CHUNK_LINE_LENGTH] = "";
		char linebuf[SNM_MAX_CHUNK_LINE_LENGTH] = "";
		while (ReadTrimmedLine(ctx, linebuf, sizeof(linebuf))) {
			if (linebuf[0] == '>')
				break;
			if (profileName[0] == '\0')
				lstrcpyn(profileName, linebuf, sizeof(profileName));
		}
		if (profileName[0]) {
			g_activeRoleProfile.Get()->Set(profileName);
			WDL_FastString path(RoleProfilePath(profileName, false).Get());
			LoadRolesFile(path.Get());
		}
		return true;
	}
	else if (!strcmp(lp.gettoken_str(0), "<S&M_PROJNOTES"))
	{
		WDL_FastString notes;
		ExtensionConfigToString(&notes, ctx);

		static char s_chunkBuf[MAX_HELP_LENGTH];
		s_chunkBuf[0] = '\0';
		GetStringFromNotesChunk(&notes, s_chunkBuf, MAX_HELP_LENGTH);

		g_prjNotes.Get()->Set(s_chunkBuf);
		return true;
	} 
	else if (!strcmp(lp.gettoken_str(0), "<S&M_TRACKNOTES"))
	{
		WDL_FastString notes;
		ExtensionConfigToString(&notes, ctx);

		static char s_chunkBuf[MAX_HELP_LENGTH];
		s_chunkBuf[0] = '\0';
		if (GetStringFromNotesChunk(&notes, s_chunkBuf, MAX_HELP_LENGTH))
		{
			GUID g;
			stringToGuid(lp.gettoken_str(1), &g);
			g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(p, &g, s_chunkBuf));
		}
		return true;
	}
	else if (!strcmp(lp.gettoken_str(0), "<S&M_SUBTITLE"))
	{
    int id = lp.gettoken_int(1);
    bool isNewFormat = (lp.getnumtokens() >= 4);
    double startTime = isNewFormat ? lp.gettoken_float(2) : 0.0;
    double endTime = isNewFormat ? lp.gettoken_float(3) : 0.0;

			WDL_FastString notes;
			ExtensionConfigToString(&notes, ctx);

			static char s_chunkBuf[MAX_HELP_LENGTH];
			s_chunkBuf[0] = '\0';
    char actor[256] = "";

    if (isNewFormat) {
      const char *notesPtr = notes.Get();
      const char *newlinePos = strchr(notesPtr, '\n');
      if (newlinePos) {
        int actorLen = (int) (newlinePos - notesPtr);
        if (actorLen >= (int) sizeof(actor))
          actorLen = (int) sizeof(actor) - 1;
        if (actorLen > 0)
          memcpy(actor, notesPtr, actorLen);
        actor[actorLen] = 0;
		}
    } else {
      actor[0] = '\0';
    }

    if (GetStringFromNotesChunk(&notes, s_chunkBuf, MAX_HELP_LENGTH)) {
      SNM_RegionSubtitle *sub = NULL;
      for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++) {
        if (g_pRegionSubs.Get()->Get(i)->GetId() == id) {
          sub = g_pRegionSubs.Get()->Get(i);
          break;
        }
      }
      if (sub) {
        sub->SetActor(actor);
        sub->SetNotes(s_chunkBuf);
        if (isNewFormat)
          sub->SetTimes(startTime, endTime);
      } else {
        sub = new SNM_RegionSubtitle(p, id, s_chunkBuf);
        sub->SetActor(actor);
        if (isNewFormat)
          sub->SetTimes(startTime, endTime);
        g_pRegionSubs.Get()->Add(sub);
      }
    }
    g_pendingRegionRecreation = true;
    return true;
  } else if (!strcmp(lp.gettoken_str(0), "<S&M_ACTOR")) {
    bool enabled = (lp.gettoken_int(1) == 1);
    int color = lp.gettoken_int(2);
    bool hasCustomColor = (lp.gettoken_int(3) == 1);

    char actorName[SNM_MAX_CHUNK_LINE_LENGTH] = "";
    char linkedActor[SNM_MAX_CHUNK_LINE_LENGTH] = "";
    bool hasLinked = false;

    char linebuf[SNM_MAX_CHUNK_LINE_LENGTH] = "";
    while (ReadTrimmedLine(ctx, linebuf, sizeof(linebuf))) {
      if (linebuf[0] == '>')
        break;
      if (actorName[0] == '\0') {
        lstrcpyn(actorName, linebuf, sizeof(actorName));
      } else if (!hasLinked) {
        lstrcpyn(linkedActor, linebuf, sizeof(linkedActor));
        hasLinked = true;
      }
    }

    if (actorName[0]) {
      SNM_Actor *actor = FindActorInList(g_actors.Get(), actorName);
      if (!actor) {
        actor = new SNM_Actor(actorName, color);
        g_actors.Get()->Add(actor);
      }
      actor->SetEnabled(enabled);
      actor->SetColor(color);
      actor->SetHasCustomColor(hasCustomColor);
      if (hasLinked)
        actor->SetLinkedActorName(linkedActor);
    }
    g_pendingRegionRecreation = true;
    return true;
	} else if (!strcmp(lp.gettoken_str(0), "<S&M_HIGHLIGHTED_ACTOR")) {
    char actorName[SNM_MAX_CHUNK_LINE_LENGTH] = "";
    char linebuf[SNM_MAX_CHUNK_LINE_LENGTH] = "";
    while (ReadTrimmedLine(ctx, linebuf, sizeof(linebuf))) {
      if (linebuf[0] == '>')
        break;
      if (actorName[0] == '\0')
        lstrcpyn(actorName, linebuf, sizeof(actorName));
    }
    if (actorName[0]) {
      g_highlightedActor.Get()->Set(actorName);
    }
    return true;
  } else if (!strcmp(lp.gettoken_str(0), "<S&M_VIDEO_OVERLAY")) {
    if (lp.getnumtokens() >= 7) {
      g_videoOverlayEnabled = (lp.gettoken_int(1) != 0);
      g_videoOverlayIncludeActor = (lp.gettoken_int(2) != 0);
      double sc = lp.gettoken_float(3);
      if (sc > 10.0) sc = 1.0;
      if (sc >= 0.4 && sc <= 2.5) g_videoOverlayScale = sc;
      else g_videoOverlayScale = 1.0;
      double py = lp.gettoken_float(4);
      if (py > 1.0) py /= 100.0;
      if (py >= 0.0 && py <= 1.0) g_videoOverlayPosY = py;
      else g_videoOverlayPosY = 0.88;
      double mw = lp.gettoken_float(5);
      if (mw > 1.0) mw /= 100.0;
      if (mw >= 0.2 && mw <= 1.0) g_videoOverlayMaxWidth = mw;
      else g_videoOverlayMaxWidth = 0.90;
      double bg = lp.gettoken_float(6);
      if (bg > 1.0) bg /= 100.0;
      if (bg >= 0.0 && bg <= 1.0) g_videoOverlayBgAlpha = bg;
      else g_videoOverlayBgAlpha = 0.55;
      if (lp.getnumtokens() >= 8) {
        int cm = lp.gettoken_int(7);
        if (cm >= 0 && cm <= 2)
          g_videoOverlayColorMode = cm;
      }
    }
    char linebuf[SNM_MAX_CHUNK_LINE_LENGTH] = "";
    while (ReadTrimmedLine(ctx, linebuf, sizeof(linebuf))) {
      if (linebuf[0] == '>')
        break;
    }
    g_pendingRegionRecreation = true;
    return true;
  }
	return false;
}

static void SaveExtensionConfig(ProjectStateContext *ctx, bool isUndo, struct project_config_extension_t *reg)
{
	if (isUndo)
		return;

	char line[SNM_MAX_CHUNK_LINE_LENGTH] = "";
	char strId[128] = "";
	WDL_FastString formatedNotes;

	if (!isUndo && IsActiveProjectInLoadSave() &&
	    (g_actors.Get()->GetSize() || g_importedRoleActors.Get()->GetSize()))
		SaveActiveRoleProfile();
	if (g_activeRoleProfile.Get()->GetLength()) {
		ctx->AddLine("<S&M_ROLE_PROFILE");
		ctx->AddLine("%s", g_activeRoleProfile.Get()->Get());
		ctx->AddLine(">");
	}
	if (g_highlightedActor.Get()->GetLength()) {
		ctx->AddLine("<S&M_HIGHLIGHTED_ACTOR");
		ctx->AddLine("%s", g_highlightedActor.Get()->Get());
		ctx->AddLine(">");
	}
	if (g_videoOverlayEnabled) {
		ctx->AddLine("<S&M_VIDEO_OVERLAY %d %d %.2f %.2f %.2f %.2f %d",
			g_videoOverlayEnabled ? 1 : 0,
			g_videoOverlayIncludeActor ? 1 : 0,
			g_videoOverlayScale,
			g_videoOverlayPosY,
			g_videoOverlayMaxWidth,
			g_videoOverlayBgAlpha,
			g_videoOverlayColorMode);
		ctx->AddLine(">");
	}

	// save project notes
	if (g_prjNotes.Get()->GetLength())
	{
		strcpy(line, "<S&M_PROJNOTES\n|");
		if (GetNotesChunkFromString(g_prjNotes.Get()->Get(), &formatedNotes, line))
			StringToExtensionConfig(&formatedNotes, ctx);
	}

	// save track notes
	for (int i=0; i<g_SNM_TrackNotes.Get()->GetSize(); i++)
	{
		if (SNM_TrackNotes* tn = g_SNM_TrackNotes.Get()->Get(i))
		{
			MediaTrack* tr = tn->GetTrack();
			if (tr && tn->GetNotesLength() > 0)
			{
				guidToString((GUID*)tn->GetGUID(), strId);
				if (snprintfStrict(line, sizeof(line), "<S&M_TRACKNOTES %s\n|", strId) > 0)
					if (GetNotesChunkFromString(tn->GetNotes(), &formatedNotes, line))
						StringToExtensionConfig(&formatedNotes, ctx);
			}
			else
				g_SNM_TrackNotes.Get()->Delete(i--, true);
		}
	}

	// write global notes to file (only when cur. project is saved)
	if (!isUndo && IsActiveProjectInLoadSave())
	{
		WriteGlobalNotesToFile();
		
	}

	// save region/marker subs
	for (int i=0; i<g_pRegionSubs.Get()->GetSize(); i++)
	{
		if (SNM_RegionSubtitle* sub = g_pRegionSubs.Get()->Get(i))
		{
			if (!sub->GetNotesLength())
        continue;
      double startTime = sub->GetStartTime();
      double endTime = sub->GetEndTime();
      if (sub->IsValid())
        EnumMarkerRegionById(NULL, sub->GetId(), NULL, &startTime, &endTime, NULL, NULL, NULL);
      const char *act = sub->GetActor() ? sub->GetActor() : "";
      if (snprintfStrict(line, sizeof(line),
                         "<S&M_SUBTITLE %d %.6f %.6f\n%s\n|", sub->GetId(),
                         startTime,
                         endTime,
                         act) > 0)
					if (GetNotesChunkFromString(sub->GetNotes(), &formatedNotes, line))
						StringToExtensionConfig(&formatedNotes, ctx);
			}
  }

  for (int i = 0; i < g_actors.Get()->GetSize(); i++) {
    if (SNM_Actor *actor = g_actors.Get()->Get(i)) {
      if (snprintfStrict(line,
                         sizeof(line),
                         "<S&M_ACTOR %d %d %d",
                         actor->IsEnabled() ? 1 : 0,
                         actor->GetColor(),
                         actor->HasCustomColor() ? 1 : 0) > 0) {
        ctx->AddLine("%s", line);
        ctx->AddLine("%s", actor->GetName());
        if (actor->HasLinkedActor())
          ctx->AddLine("%s", actor->GetLinkedActorName());
        ctx->AddLine(">");
			}
		}
	}
}

static void BeginLoadProjectState(bool isUndo, struct project_config_extension_t *reg)
{
  ReNotes_LogMsg("BeginLoadProjectState: isUndo=%d", isUndo ? 1 : 0);
  if (isUndo) {
    g_lastMarkerRegionId = -1;
    g_lastMarkerPos = -1.0;
    return;
  }

  g_pendingRegionRecreation = !isUndo;
	g_prjNotes.Cleanup();
	g_prjNotes.Get()->Set("");

	g_SNM_TrackNotes.Cleanup();
	g_SNM_TrackNotes.Get()->Empty(true);

	g_pRegionSubs.Cleanup();
	g_pRegionSubs.Get()->Empty(true);

  g_actors.Cleanup();
  g_actors.Get()->Empty(true);
  g_importedRoleActors.Cleanup();
  g_importedRoleActors.Get()->Empty(true);
  g_activeRoleProfile.Cleanup();
  g_activeRoleProfile.Get()->Set("");

  if (!isUndo)
    g_videoOverlayEnabled = (GetPrivateProfileInt(NOTES_INI_SEC, "VideoOverlay", 0, g_SNM_IniFn.Get()) == 1);

	// g_globalNotes is loaded in NotesInit()
}

static project_config_extension_t s_projectconfig = {
	ProcessExtensionLine, SaveExtensionConfig, BeginLoadProjectState, NULL
};


///////////////////////////////////////////////////////////////////////////////

void NotesSetTrackTitle()
{
	if (g_notesType == SNM_NOTES_TRACK)
		if (NotesWnd* w = g_notesWndMgr.Get())
			w->RefreshGUI();
}

// this is our only notification of active project tab change, so update everything
// (ScheduledJob because of multi-notifs)
void NotesSetTrackListChange() {
	ScheduledJob::Schedule(new NotesUpdateJob(SNM_SCHEDJOB_ASYNC_DELAY_OPT));
}


///////////////////////////////////////////////////////////////////////////////

int NotesInit()
{
	lstrcpyn(g_lastImportSubFn, GetResourcePath(), sizeof(g_lastImportSubFn));
	lstrcpyn(g_lastRolesFn, GetResourcePath(), sizeof(g_lastRolesFn));

	// load prefs 
	g_notesType = GetPrivateProfileInt(NOTES_INI_SEC, "Type", SNM_NOTES_RGN_SUB, g_SNM_IniFn.Get());
	g_locked = (GetPrivateProfileInt(NOTES_INI_SEC, "Lock", 1, g_SNM_IniFn.Get()) == 1);
	GetPrivateProfileString(NOTES_INI_SEC, "BigFontName", SNM_DYN_FONT_NAME, g_notesBigFontName, sizeof(g_notesBigFontName), g_SNM_IniFn.Get());
	g_wrapText=(GetPrivateProfileInt(NOTES_INI_SEC, "WrapText", 0, g_SNM_IniFn.Get()) == 1);
  g_coloredRegions = (GetPrivateProfileInt(NOTES_INI_SEC, "ColoredRegions", 1, g_SNM_IniFn.Get()) ==
    1);
  g_displayActorInPrefix =
      (GetPrivateProfileInt(NOTES_INI_SEC, "DisplayActorInPrefix", 1, g_SNM_IniFn.Get()) == 1);
  g_hideRegions = (GetPrivateProfileInt(NOTES_INI_SEC, "HideRegions", 0, g_SNM_IniFn.Get()) == 1);
  g_hideActorList = (GetPrivateProfileInt(NOTES_INI_SEC, "HideActorList", 0, g_SNM_IniFn.Get()) == 1);
  g_copyTimeFormat = GetPrivateProfileInt(NOTES_INI_SEC, "CopyTimeFormat", 0, g_SNM_IniFn.Get());
  if (g_copyTimeFormat < 0 || g_copyTimeFormat > 2)
    g_copyTimeFormat = 0;
  g_autoCheckUpdates = (GetPrivateProfileInt(NOTES_INI_SEC, "AutoCheckUpdates", 1, g_SNM_IniFn.Get()) == 1);
  if (g_autoCheckUpdates) {
    int lastCheck = GetPrivateProfileInt(NOTES_INI_SEC, "LastUpdateCheck", 0, g_SNM_IniFn.Get());
    time_t now = time(NULL);
    if (lastCheck == 0 || (now > 0 && (now - lastCheck) > 24 * 3600)) {
      char nowBuf[32] = "";
      snprintf(nowBuf, sizeof(nowBuf), "%d", static_cast<int>(now));
      WritePrivateProfileString(NOTES_INI_SEC, "LastUpdateCheck", nowBuf, g_SNM_IniFn.Get());
      CheckReNotesUpdates(false);
    }
  }

  g_videoOverlayEnabled = (GetPrivateProfileInt(NOTES_INI_SEC, "VideoOverlay", 1, g_SNM_IniFn.Get()) == 1);
  g_videoOverlayIncludeActor = (GetPrivateProfileInt(NOTES_INI_SEC, "VideoOverlayActor", 1, g_SNM_IniFn.Get()) == 1);
  g_videoOverlayColorMode = GetPrivateProfileInt(NOTES_INI_SEC, "VideoOverlayColorMode", 1, g_SNM_IniFn.Get());
  if (g_videoOverlayColorMode < 0 || g_videoOverlayColorMode > 2)
    g_videoOverlayColorMode = 1;
  char voBuf[64] = "";
  if (GetPrivateProfileString(NOTES_INI_SEC, "VideoOverlayScale", "", voBuf, sizeof(voBuf), g_SNM_IniFn.Get()) > 0) {
    double v = atof(voBuf);
    if (v > 10.0) v = 1.0;
    if (v >= 0.4 && v <= 2.5) g_videoOverlayScale = v;
    else g_videoOverlayScale = 1.0;
  } else {
    g_videoOverlayScale = 1.0;
  }
  if (GetPrivateProfileString(NOTES_INI_SEC, "VideoOverlayPosY", "", voBuf, sizeof(voBuf), g_SNM_IniFn.Get()) > 0) {
    double v = atof(voBuf);
    if (v > 1.0) v /= 100.0;
    if (v >= 0.0 && v <= 1.0) g_videoOverlayPosY = v;
    else g_videoOverlayPosY = 0.88;
  } else {
    g_videoOverlayPosY = 0.88;
  }
  if (GetPrivateProfileString(NOTES_INI_SEC, "VideoOverlayMaxWidth", "", voBuf, sizeof(voBuf), g_SNM_IniFn.Get()) > 0) {
    double v = atof(voBuf);
    if (v > 1.0) v /= 100.0;
    if (v >= 0.2 && v <= 1.0) g_videoOverlayMaxWidth = v;
    else g_videoOverlayMaxWidth = 0.90;
  } else {
    g_videoOverlayMaxWidth = 0.90;
  }
  if (GetPrivateProfileString(NOTES_INI_SEC, "VideoOverlayBgAlpha", "", voBuf, sizeof(voBuf), g_SNM_IniFn.Get()) > 0) {
    double v = atof(voBuf);
    if (v > 1.0) v /= 100.0;
    if (v >= 0.0 && v <= 1.0) g_videoOverlayBgAlpha = v;
    else g_videoOverlayBgAlpha = 0.55;
  } else {
    g_videoOverlayBgAlpha = 0.55;
  }

	// Read global notes from the ReNotes file, falling back to the legacy SWS filename.
	WDL_FastString filePath;
	filePath.SetFormatted(SNM_MAX_PATH, "%s/SWS_Global ReNotes.txt", GetResourcePath());
	WDL_FileRead infile(filePath.Get(), 0, 65536);
  int infileSize = static_cast<int>(infile.GetSize());
  if (infileSize < 0) {
	filePath.SetFormatted(SNM_MAX_PATH, "%s/SWS_Global notes.txt", GetResourcePath());
    WDL_FileRead oldFile(filePath.Get(), 0, 65536);
    infileSize = static_cast<int>(oldFile.GetSize());
	if(infileSize >= 0 && g_globalNotes.SetLen(infileSize, true))
      oldFile.Read(const_cast<char *>(g_globalNotes.Get()), infileSize);
  } else if(infileSize >= 0 && g_globalNotes.SetLen(infileSize, true))
		infile.Read(const_cast<char *>(g_globalNotes.Get()), infileSize);
	
	// instanciate the window if needed, can be NULL
	g_notesWndMgr.Init();

	if (!plugin_register("projectconfig", &s_projectconfig))
		return 0;

	return 1;
}

void NotesExit()
{
	plugin_register("-projectconfig", &s_projectconfig);

	char tmp[4] = "";
	if (snprintfStrict(tmp, sizeof(tmp), "%d", g_notesType) > 0)
		WritePrivateProfileString(NOTES_INI_SEC, "Type", tmp, g_SNM_IniFn.Get()); 
	WritePrivateProfileString(NOTES_INI_SEC, "Lock", g_locked?"1":"0", g_SNM_IniFn.Get()); 
	WritePrivateProfileString(NOTES_INI_SEC, "BigFontName", g_notesBigFontName, g_SNM_IniFn.Get());
	WritePrivateProfileString(NOTES_INI_SEC, "WrapText", g_wrapText?"1":"0", g_SNM_IniFn.Get());
  WritePrivateProfileString(NOTES_INI_SEC,
                            "ColoredRegions",
                            g_coloredRegions ? "1" : "0",
                            g_SNM_IniFn.Get());
	WritePrivateProfileString(NOTES_INI_SEC,
                            "DisplayActorInPrefix",
                            g_displayActorInPrefix ? "1" : "0",
                            g_SNM_IniFn.Get());
  WritePrivateProfileString(NOTES_INI_SEC,
                            "HideRegions",
                            g_hideRegions ? "1" : "0",
                            g_SNM_IniFn.Get());
  WritePrivateProfileString(NOTES_INI_SEC,
                            "HideActorList",
                            g_hideActorList ? "1" : "0", g_SNM_IniFn.Get());
  char timeFmtBuf[4] = "";
  if (snprintfStrict(timeFmtBuf, sizeof(timeFmtBuf), "%d", g_copyTimeFormat) > 0)
    WritePrivateProfileString(NOTES_INI_SEC, "CopyTimeFormat", timeFmtBuf, g_SNM_IniFn.Get());
  WritePrivateProfileString(NOTES_INI_SEC,
                            "AutoCheckUpdates",
                            g_autoCheckUpdates ? "1" : "0", g_SNM_IniFn.Get());

  WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlay", g_videoOverlayEnabled ? "1" : "0", g_SNM_IniFn.Get());
  WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlayActor", g_videoOverlayIncludeActor ? "1" : "0", g_SNM_IniFn.Get());
  char cmBuf[4] = "";
  if (snprintfStrict(cmBuf, sizeof(cmBuf), "%d", g_videoOverlayColorMode) > 0)
    WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlayColorMode", cmBuf, g_SNM_IniFn.Get());
  char voBuf[32] = "";
  if (snprintfStrict(voBuf, sizeof(voBuf), "%.2f", g_videoOverlayScale) > 0)
    WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlayScale", voBuf, g_SNM_IniFn.Get());
  if (snprintfStrict(voBuf, sizeof(voBuf), "%.2f", g_videoOverlayPosY) > 0)
    WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlayPosY", voBuf, g_SNM_IniFn.Get());
  if (snprintfStrict(voBuf, sizeof(voBuf), "%.2f", g_videoOverlayMaxWidth) > 0)
    WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlayMaxWidth", voBuf, g_SNM_IniFn.Get());
  if (snprintfStrict(voBuf, sizeof(voBuf), "%.2f", g_videoOverlayBgAlpha) > 0)
    WritePrivateProfileString(NOTES_INI_SEC, "VideoOverlayBgAlpha", voBuf, g_SNM_IniFn.Get());

  if (s_videoSyncCmdId > 0 && AddRemoveReaScript) {
    char luaPath[SNM_MAX_PATH] = "";
    snprintf(luaPath, sizeof(luaPath), "%s/ReNotes_VideoSync.lua", GetResourcePath());
    AddRemoveReaScript(false, 0, luaPath, true);
    s_videoSyncCmdId = 0;
  }

	g_notesWndMgr.Delete();
}

void OpenNotes(COMMAND_T* _ct)
{
	if (NotesWnd* w = g_notesWndMgr.Create())
	{
		int newType = (int)_ct->user; // -1 means toggle current type
		if (newType == -1)
			newType = g_notesType;

		w->Show(g_notesType == newType /* i.e toggle */, true);

		w->SetType(newType);

		if (!g_locked)
			SetFocus(w->GetEditControl());
	}
}

int IsNotesDisplayed(COMMAND_T* _ct)
{
	if (NotesWnd* w = g_notesWndMgr.Get())
		return w->IsWndVisible();
	return 0;
}

void ToggleNotesLock(COMMAND_T*) {
	if (NotesWnd* w = g_notesWndMgr.Get())
		w->ToggleLock();
}

int IsNotesLocked(COMMAND_T*) {
	return g_locked;
}

void ToggleColoredRegions(COMMAND_T *) {
  g_coloredRegions = !g_coloredRegions;
  UpdateRegionColors();
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->RefreshGUI();
}

int IsColoredRegions(COMMAND_T *) {
  return g_coloredRegions;
}

void CleanOrphanSubs() {
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  for (int i = 0; i < subs->GetSize(); i++) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (!sub) continue;
    const char *act = sub->GetActor();
    if (act && *act && strcmp(act, "?") != 0 && !FindActor(act)) {
      sub->SetActor("");
    }
  }
}

void HideAllRegions() {
  InternalMkrRgnGuard mkrGuard;
  CleanOrphanSubs();
  WDL_PtrList_DOD<SNM_RegionSubtitle> *subs = g_pRegionSubs.Get();
  PreventUIRefresh(1);
  for (int i = 0; i < subs->GetSize(); i++) {
    SNM_RegionSubtitle *sub = subs->Get(i);
    if (sub && sub->IsValid()) {
      double pos, endPos;
      if (EnumMarkerRegionById(NULL, sub->GetId(), NULL, &pos, &endPos, NULL, NULL, NULL) >= 0)
        sub->SetTimes(pos, endPos);
      int idx = GetMarkerRegionIndexFromId(NULL, sub->GetId());
      if (idx >= 0)
        DeleteProjectMarkerByIndex(NULL, idx);
      sub->SetId(-1);
    }
  }
  PreventUIRefresh(-1);
}

void ShowAllRegions() {
  InternalMkrRgnGuard mkrGuard;
  CleanOrphanSubs();
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  PreventUIRefresh(1);
  for (int i = 0; i < actors->GetSize(); i++) {
    if (actors->Get(i)->IsEnabled() && strcmp(actors->Get(i)->GetName(), "?") != 0)
      RecreateActorRegions(actors->Get(i)->GetName());
  }
  RecreateNoActorRegions();
  PreventUIRefresh(-1);
  UpdateTimeline();
}

void ToggleHideRegions(COMMAND_T *) {
  g_hideRegions = !g_hideRegions;
  if (g_hideRegions)
    HideAllRegions();
  else
    ShowAllRegions();
  g_lastMarkerPos = -1.0;
  UpdateTimeline();
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->RefreshGUI();
}

void ToggleHideActorList(COMMAND_T *) {
  g_hideActorList = !g_hideActorList;
  if (NotesWnd *w = g_notesWndMgr.Get()) {
    HWND hwnd = w->GetHWND();
    RECT r;
    GetClientRect(hwnd, &r);
    SendMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
  }
}

void ToggleDisplayActorInPrefix(COMMAND_T *) {
  g_displayActorInPrefix = !g_displayActorInPrefix;
  UpdateRegionColors();
  if (NotesWnd *w = g_notesWndMgr.Get()) {
    w->ForceUpdateRgnSub();
    w->RefreshGUI();
  }
}

int IsHideRegions(COMMAND_T *) {
  return g_hideRegions;
}

int IsHideActorList(COMMAND_T *) {
  return g_hideActorList;
}

int IsDisplayActorInPrefix(COMMAND_T *) {
  return g_displayActorInPrefix;
}

void CopyMarkersAction(COMMAND_T *) {
  CopyMarkersToClipboard(GetMainHwnd());
}

void CopyMarkersWithFramesAction(COMMAND_T *) {
  CopyMarkersToClipboard(GetMainHwnd(), SNM_TIME_FORMAT_FRAMES);
}

void CopyMarkersNoFramesAction(COMMAND_T *) {
  CopyMarkersToClipboard(GetMainHwnd(), SNM_TIME_FORMAT_NO_FRAMES);
}

void ToggleCopyFramesAction(COMMAND_T *) {
  g_copyTimeFormat = (g_copyTimeFormat == SNM_TIME_FORMAT_FRAMES) ? SNM_TIME_FORMAT_NO_FRAMES : SNM_TIME_FORMAT_FRAMES;
  char tmp[4] = "";
  if (snprintfStrict(tmp, sizeof(tmp), "%d", g_copyTimeFormat) > 0)
    WritePrivateProfileString(NOTES_INI_SEC, "CopyTimeFormat", tmp, g_SNM_IniFn.Get());
}

int IsCopyFramesAction(COMMAND_T *) {
  return (g_copyTimeFormat == SNM_TIME_FORMAT_FRAMES) ? 1 : 0;
}

struct ReNotesUpdateState {
  bool interactive;
};

static unsigned WINAPI ReNotesUpdateThreadProc(void *param)
{
  ReNotesUpdateState *st = reinterpret_cast<ReNotesUpdateState *>(param);
  bool interactive = st ? st->interactive : false;
  delete st;

  int latestVersion = 0;
  std::string tagName;
  std::string htmlUrl = "https://github.com/ivangund/sws-renotes/releases";
  std::string downloadUrl;
  std::string json;

#ifdef _WIN32
  HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) ReNotes-Updater",
                                   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
  if (hSession) {
    HINTERNET hConnect = WinHttpConnect(hSession, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
      HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
                                             L"/ivangund/sws-renotes/releases/latest",
                                             NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
      if (hRequest) {
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, NULL)) {
          DWORD statusCode = 0;
          DWORD statusSize = sizeof(statusCode);
          if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
            if (statusCode == 301 || statusCode == 302) {
              DWORD locSize = 0;
              WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &locSize, WINHTTP_NO_HEADER_INDEX);
              if (locSize > 0) {
                std::vector<wchar_t> locBuf(locSize / sizeof(wchar_t) + 1, 0);
                if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, locBuf.data(), &locSize, WINHTTP_NO_HEADER_INDEX)) {
                  int wlen = (int)wcslen(locBuf.data());
                  int utf8Len = WideCharToMultiByte(CP_UTF8, 0, locBuf.data(), wlen, NULL, 0, NULL, NULL);
                  if (utf8Len > 0) {
                    std::string loc(utf8Len, 0);
                    WideCharToMultiByte(CP_UTF8, 0, locBuf.data(), wlen, &loc[0], utf8Len, NULL, NULL);
                    size_t tagPos = loc.rfind("/tag/");
                    if (tagPos != std::string::npos) {
                      tagName = loc.substr(tagPos + 5);
                      htmlUrl = loc;
                      const char *p = tagName.c_str();
                      while (*p && !isdigit(static_cast<unsigned char>(*p))) p++;
                      if (*p) latestVersion = atoi(p);
                    }
                  }
                }
              }
            }
          }
        }
        WinHttpCloseHandle(hRequest);
      }

      if (!tagName.empty()) {
        std::wstring assetsPath = L"/ivangund/sws-renotes/releases/expanded_assets/" + std::wstring(tagName.begin(), tagName.end());
        HINTERNET hAssetReq = WinHttpOpenRequest(hConnect, L"GET",
                                                assetsPath.c_str(),
                                                NULL, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (hAssetReq) {
          if (WinHttpSendRequest(hAssetReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hAssetReq, NULL)) {
            std::string assetHtml;
            DWORD dwSize = 0;
            while (WinHttpQueryDataAvailable(hAssetReq, &dwSize) && dwSize > 0) {
              std::vector<char> buf(dwSize + 1, 0);
              DWORD dwDownloaded = 0;
              if (WinHttpReadData(hAssetReq, buf.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
                assetHtml.append(buf.data(), dwDownloaded);
              } else {
                break;
              }
            }
            size_t exePos = assetHtml.find(".exe");
            if (exePos != std::string::npos) {
              size_t hrefStart = assetHtml.rfind("href=\"", exePos);
              if (hrefStart != std::string::npos) {
                hrefStart += 6;
                std::string href = assetHtml.substr(hrefStart, exePos + 4 - hrefStart);
                if (href.rfind("http", 0) == 0)
                  downloadUrl = href;
                else
                  downloadUrl = "https://github.com" + href;
              }
            }
          }
          WinHttpCloseHandle(hAssetReq);
        }
      }

      WinHttpCloseHandle(hConnect);
    }

    if (tagName.empty()) {
      HINTERNET hApiConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
      if (hApiConnect) {
        HINTERNET hApiReq = WinHttpOpenRequest(hApiConnect, L"GET",
                                               L"/repos/ivangund/sws-renotes/releases/latest",
                                               NULL, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        if (hApiReq) {
          WinHttpAddRequestHeaders(hApiReq,
                                   L"User-Agent: ReNotes-Updater\r\nAccept: application/vnd.github.v3+json\r\n",
                                   -1, WINHTTP_ADDREQ_FLAG_ADD);
          if (WinHttpSendRequest(hApiReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hApiReq, NULL)) {
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            if (WinHttpQueryHeaders(hApiReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) &&
                statusCode == 200) {
              DWORD dwSize = 0;
              while (WinHttpQueryDataAvailable(hApiReq, &dwSize) && dwSize > 0) {
                std::vector<char> buf(dwSize + 1, 0);
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hApiReq, buf.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
                  json.append(buf.data(), dwDownloaded);
                } else {
                  break;
                }
              }
            }
          }
          WinHttpCloseHandle(hApiReq);
        }
        WinHttpCloseHandle(hApiConnect);
      }
    }

    WinHttpCloseHandle(hSession);
  }
#else
  FILE *pipe = popen("curl -s -L -I https://github.com/ivangund/sws-renotes/releases/latest | grep -i Location", "r");
  if (pipe) {
    char buf[512];
    if (fgets(buf, sizeof(buf), pipe)) {
      std::string line = buf;
      size_t tagPos = line.rfind("/tag/");
      if (tagPos != std::string::npos) {
        tagName = line.substr(tagPos + 5);
        while (!tagName.empty() && (tagName.back() == '\r' || tagName.back() == '\n' || tagName.back() == ' '))
          tagName.pop_back();
        htmlUrl = "https://github.com/ivangund/sws-renotes/releases/tag/" + tagName;
        const char *p = tagName.c_str();
        while (*p && !isdigit(static_cast<unsigned char>(*p))) p++;
        if (*p) latestVersion = atoi(p);
      }
    }
    pclose(pipe);
  }
#endif

  if (!json.empty() && tagName.empty()) {
    size_t tagPos = json.find("\"tag_name\"");
    if (tagPos != std::string::npos) {
      size_t colon = json.find(':', tagPos);
      if (colon != std::string::npos) {
        size_t q1 = json.find('\"', colon);
        if (q1 != std::string::npos) {
          size_t q2 = json.find('\"', q1 + 1);
          if (q2 != std::string::npos) {
            tagName = json.substr(q1 + 1, q2 - q1 - 1);
            const char *p = tagName.c_str();
            while (*p && !isdigit(static_cast<unsigned char>(*p))) p++;
            if (*p) latestVersion = atoi(p);
          }
        }
      }
    }

    size_t htmlPos = json.find("\"html_url\"");
    if (htmlPos != std::string::npos) {
      size_t colon = json.find(':', htmlPos);
      if (colon != std::string::npos) {
        size_t q1 = json.find('\"', colon);
        if (q1 != std::string::npos) {
          size_t q2 = json.find('\"', q1 + 1);
          if (q2 != std::string::npos) {
            htmlUrl = json.substr(q1 + 1, q2 - q1 - 1);
          }
        }
      }
    }

    if (downloadUrl.empty()) {
      size_t assetPos = 0;
      while ((assetPos = json.find("\"browser_download_url\"", assetPos)) != std::string::npos) {
        size_t colon = json.find(':', assetPos);
        if (colon != std::string::npos) {
          size_t q1 = json.find('\"', colon);
          if (q1 != std::string::npos) {
            size_t q2 = json.find('\"', q1 + 1);
            if (q2 != std::string::npos) {
              std::string u = json.substr(q1 + 1, q2 - q1 - 1);
              if (u.find(".exe") != std::string::npos && u.find("Windows") != std::string::npos) {
                downloadUrl = u;
                break;
              }
            }
          }
        }
        assetPos += 22;
      }
    }
  }

  if (tagName.empty() && json.empty()) {
    if (interactive) {
      MessageBox(GetMainHwnd(),
                 __LOCALIZE("Не удалось проверить обновления.\nПроверьте подключение к интернету.", "sws_DLG_152"),
                 __LOCALIZE("ReNotes - Обновление", "sws_DLG_152"),
                 MB_OK | MB_ICONWARNING);
    }
    return 0;
  }

  int currentVer = RENOTES_VERSION;
  if (latestVersion > currentVer) {
    char msg[1024];
    snprintf(msg, sizeof(msg),
             __LOCALIZE_VERFMT("Доступна новая версия ReNotes: v%d!\n(У вас установлена версия: v%d)\n\nХотите скачать и запустить установщик сейчас?\n\n• «Да» — Скачать установщик и обновить\n• «Нет» — Открыть страницу релиза на GitHub\n• «Отмена» — Напомнить позже", "sws_DLG_152"),
             latestVersion, currentVer);

    int res = MessageBox(GetMainHwnd(), msg,
                         __LOCALIZE("ReNotes - Доступно обновление", "sws_DLG_152"),
                         MB_YESNOCANCEL | MB_ICONINFORMATION);
    if (res == IDYES) {
#ifdef _WIN32
      if (!downloadUrl.empty()) {
        char tempDir[MAX_PATH] = "";
        GetTempPathA(sizeof(tempDir), tempDir);
        char installerPath[MAX_PATH] = "";
        snprintf(installerPath, sizeof(installerPath), "%ssws-renotes-v%d-setup.exe", tempDir, latestVersion);

        HRESULT hr = URLDownloadToFileA(NULL, downloadUrl.c_str(), installerPath, 0, NULL);
        if (SUCCEEDED(hr)) {
          char readyMsg[512];
          snprintf(readyMsg, sizeof(readyMsg),
                   __LOCALIZE("Установщик успешно скачан!\n\nПожалуйста, закройте REAPER и нажмите «ОК» для запуска установщика.", "sws_DLG_152"));
          MessageBox(GetMainHwnd(), readyMsg, __LOCALIZE("ReNotes - Обновление", "sws_DLG_152"), MB_OK | MB_ICONINFORMATION);
          ShellExecuteA(NULL, "open", installerPath, NULL, NULL, SW_SHOWNORMAL);
        } else {
          ShellExecuteA(NULL, "open", htmlUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
        }
      } else {
        ShellExecuteA(NULL, "open", htmlUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
      }
#else
      ShellExecute(NULL, "open", htmlUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
#endif
    } else if (res == IDNO) {
#ifdef _WIN32
      ShellExecuteA(NULL, "open", htmlUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
      ShellExecute(NULL, "open", htmlUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
#endif
    }
  } else if (interactive) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             __LOCALIZE_VERFMT("У вас установлена актуальная версия ReNotes (v%d).", "sws_DLG_152"),
             currentVer);
    MessageBox(GetMainHwnd(), msg,
               __LOCALIZE("ReNotes - Проверка обновлений", "sws_DLG_152"),
               MB_OK | MB_ICONINFORMATION);
  }

  return 0;
}

void CheckReNotesUpdates(bool interactive)
{
  ReNotesUpdateState *st = new ReNotesUpdateState{interactive};
#ifdef _WIN32
  uintptr_t h = _beginthreadex(NULL, 0, ReNotesUpdateThreadProc, st, 0, NULL);
  if (h)
    CloseHandle((HANDLE)h);
  else
    delete st;
#else
  std::thread t(ReNotesUpdateThreadProc, st);
  t.detach();
#endif
}

void CheckReNotesUpdatesAction(COMMAND_T *)
{
  CheckReNotesUpdates(true);
}

void ToggleAutoCheckUpdatesAction(COMMAND_T *)
{
  g_autoCheckUpdates = !g_autoCheckUpdates;
  WritePrivateProfileString(NOTES_INI_SEC, "AutoCheckUpdates", g_autoCheckUpdates ? "1" : "0", g_SNM_IniFn.Get());
}

int IsAutoCheckUpdatesAction(COMMAND_T *)
{
  return g_autoCheckUpdates ? 1 : 0;
}

void CopyRoleDistributionAction(COMMAND_T *) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  WDL_PtrList_DOD<WDL_FastString> linkedNames;
  WDL_PtrList_DOD<WDL_FastString> charLists;
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *a = actors->Get(i);
    if (!a->HasLinkedActor()) continue;
    const char *linked = a->GetLinkedActorName();
    int idx = -1;
    for (int j = 0; j < linkedNames.GetSize(); j++) {
      if (!strcmp(linkedNames.Get(j)->Get(), linked)) {
        idx = j;
        break;
      }
    }
    if (idx < 0) {
      linkedNames.Add(new WDL_FastString(linked));
      charLists.Add(new WDL_FastString());
      idx = linkedNames.GetSize() - 1;
    }
    charLists.Get(idx)->AppendFormatted(512, "- %s\n", a->GetName());
  }
  if (!linkedNames.GetSize()) {
    MessageBox(GetMainHwnd(),
               __LOCALIZE("Нет привязанных актёров.", "sws_DLG_152"),
               __LOCALIZE("ReNotes", "sws_DLG_152"),
               MB_OK);
    return;
  }
  WDL_FastString output;
  for (int i = 0; i < linkedNames.GetSize(); i++) {
    if (i > 0) output.Append("\n");
    output.AppendFormatted(512, "%s:\n", linkedNames.Get(i)->Get());
    output.Append(charLists.Get(i)->Get());
  }
  if (output.GetLength() > 0 && output.Get()[output.GetLength() - 1] == '\n')
    output.DeleteSub(output.GetLength() - 1, 1);
  CF_SetClipboard(output.Get());
}

void ClearAllSubtitlesAction(COMMAND_T *) {
  ClearAllSubtitles();
  SyncSubtitleItemsTrack(false);
  if (NotesWnd *w = g_notesWndMgr.Get()) {
    w->RefreshActorList();
    w->Update(true);
  }
}

void ClearHighlightRolesAction(COMMAND_T *) {
  g_highlightedActor.Get()->Set("");
  UpdateRegionColors();
  UpdateTimeline();
  SyncSubtitleItemsTrack(false);
  if (NotesWnd *w = g_notesWndMgr.Get()) {
    w->RefreshActorList();
    w->RefreshGUI();
  }
  MarkProjectDirty(NULL);
}

void EnableAllActors(COMMAND_T *) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *actor = actors->Get(i);
    if (!actor->IsEnabled()) {
      actor->SetEnabled(true);
      RecreateActorRegions(actor->GetName());
    }
  }
  if (g_hideRegions)
    g_lastMarkerPos = -1.0;
  SyncSubtitleItemsTrack(false);
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->RefreshActorList();
  if (g_videoOverlayEnabled)
    SyncSubtitlesToVideoOverlay();
  MarkProjectDirty(NULL);
}

void DisableAllActors(COMMAND_T *) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  for (int i = 0; i < actors->GetSize(); i++) {
    SNM_Actor *actor = actors->Get(i);
    if (actor->IsEnabled()) {
      actor->SetEnabled(false);
      DeleteActorRegions(actor->GetName());
    }
  }
  UpdateTimeline();
  if (g_hideRegions)
    g_lastMarkerPos = -1.0;
  SyncSubtitleItemsTrack(false);
  if (NotesWnd *w = g_notesWndMgr.Get())
    w->RefreshActorList();
  if (g_videoOverlayEnabled)
    SyncSubtitlesToVideoOverlay();
  MarkProjectDirty(NULL);
}

void ImportRolesAction(COMMAND_T *) {
  if (char *fn = BrowseForFiles(
    __LOCALIZE("ReNotes - Импорт ролёвки", "sws_DLG_152"),
    g_lastRolesFn,
    NULL,
    false,
    SNM_ROLES_EXT_LIST)) {
    lstrcpyn(g_lastRolesFn, fn, sizeof(g_lastRolesFn));
    if (ImportRolesFile(fn)) {
      UpdateRegionColors();
      if (NotesWnd *w = g_notesWndMgr.Get()) {
        w->RefreshActorList();
        w->ForceUpdateRgnSub();
        w->RefreshGUI();
      }
      SyncSubtitleItemsTrack(false);
      MarkProjectDirty(NULL);
    } else
      MessageBox(GetMainHwnd(),
                 __LOCALIZE("Некорректный файл ролёвки!", "sws_DLG_152"),
                 __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                 MB_OK);
    free(fn);
  }
}

void ExportRolesAction(COMMAND_T *) {
  WDL_PtrList_DOD<SNM_Actor> *actors = g_actors.Get();
  bool hasData = (actors->GetSize() > 0 || g_importedRoleActors.Get()->GetSize() > 0);
  if (!hasData) {
    MessageBox(GetMainHwnd(),
               __LOCALIZE("Ничего не найдено для экспорта.", "sws_DLG_152"),
               __LOCALIZE("ReNotes", "sws_DLG_152"),
               MB_OK);
    return;
  }
  SaveActiveRoleProfile();
  char fn[SNM_MAX_PATH] = "";
  if (BrowseForSaveFile(
    __LOCALIZE("ReNotes - Экспорт ролёвки", "sws_DLG_152"),
    g_lastRolesFn,
    NULL,
    SNM_ROLES_EXT_LIST,
    fn,
    sizeof(fn))) {
    lstrcpyn(g_lastRolesFn, fn, sizeof(g_lastRolesFn));
    if (!ExportRolesFile(fn))
      MessageBox(GetMainHwnd(),
                 __LOCALIZE("Не удалось экспортировать ролёвку.", "sws_DLG_152"),
                 __LOCALIZE("ReNotes - Ошибка", "sws_DLG_152"),
                 MB_OK);
  }
}

/******************************************************************************
* ReaScript export #755                                                       *
******************************************************************************/
const char* NF_GetSWSTrackNotes(MediaTrack* track)
{
	SNM_TrackNotes* notes = SNM_TrackNotes::find(track);
	return notes ? notes->GetNotes() : "";
}

void NF_SetSWSTrackNotes(MediaTrack* track, const char* buf)
{
	if (!track)
		return;

	MarkProjectDirty(NULL);

	if (SNM_TrackNotes* notes = SNM_TrackNotes::find(track))
	{
		notes->SetNotes(buf);

		// update displayed text if Notes window is visible and notes for set track are displayed
		NotesWnd* w = g_notesWndMgr.Get();
		if (w && w->IsWndVisible() && g_notesType == SNM_NOTES_TRACK && g_trNote == track)
			w->SetText(buf);
		return;
	}

	// tracknote for the track doesn't exist yet, add new one 
	g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(nullptr, TrackToGuid(track), buf));
}

const char* NFDoGetSWSMarkerRegionSub(int mkrRgnIdxNumberIn)
{
	// maybe more safe version, but below also seems to work and is faster
	/*
	int idx = 0, num; 

	while ((idx = EnumProjectMarkers2(NULL, idx, NULL, NULL, NULL, NULL, &num)))
	{
		// mkrRgn exists in project
		if (idx-1 == mkrRgnIdxNumberIn)
		{
			int mkrRgnId = GetMarkerRegionIdFromIndex(NULL, idx-1); // takes zero-based idx
			for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++)
			{
				// regionSub exists
				if (mkrRgnId == g_pRegionSubs.Get()->Get(i)->GetId())
				{
					mkrRgnSubOut->Set(g_pRegionSubs.Get()->Get(i)->GetNotes());
					return mkrRgnSubOut->Get();
				}
			}
		}
	}
	*/
	
	int mkrRgnId = GetMarkerRegionIdFromIndex(NULL, mkrRgnIdxNumberIn); // takes zero-based idx
	if (mkrRgnId == -1) return "";

	for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++)
	{
		// mkrRgn sub exists
		if (mkrRgnId == g_pRegionSubs.Get()->Get(i)->GetId())
		{
			return g_pRegionSubs.Get()->Get(i)->GetNotes();
		}
	}

	return "";
}

bool NFDoSetSWSMarkerRegionSub(const char* mkrRgnSubIn, int mkrRgnIdxNumberIn)
{
	int idx = 0; bool mkrRgnExists = false;
	
	while ((idx = EnumProjectMarkers2(NULL, idx, NULL, NULL, NULL, NULL, NULL)))
	{
		if (idx - 1 == mkrRgnIdxNumberIn) // mkrRegion exists in project
		{
			mkrRgnExists = true;
		
			int mkrRgnId = GetMarkerRegionIdFromIndex(NULL, idx - 1); // takes zero-based idx
			for (int i = 0; i < g_pRegionSubs.Get()->GetSize(); i++)
			{
				if (mkrRgnId == g_pRegionSubs.Get()->Get(i)->GetId()) // mkrRgn sub exists, update it
				{
					g_pRegionSubs.Get()->Get(i)->SetNotes(mkrRgnSubIn);
					return true;
				}
			}

			// mkrRgn sub doesn't exist but marker/region is present in project, add new mkrRgn sub
			if (mkrRgnExists)
			{
				g_pRegionSubs.Get()->Add(new SNM_RegionSubtitle(nullptr, mkrRgnId, mkrRgnSubIn));
				return true;
			}
			else // mkrRgn isn't present in project
				return false;
		}
	}
	return false; // appease compilers
}

void NF_DoUpdateSWSMarkerRegionSubWindow()
{
	if (NotesWnd* w = g_notesWndMgr.Get()) {
		if (w->IsWndVisible() && g_notesType == SNM_NOTES_RGN_SUB)
			w->ForceUpdateRgnSub();
	}
}

const char* JB_GetSWSExtraProjectNotes(ReaProject* project)
{
	return g_prjNotes.Get(project)->Get();
}

void JB_SetSWSExtraProjectNotes(ReaProject* project, const char* buf)
{
	MarkProjectDirty(project);

	g_prjNotes.Get(project)->Set(buf);

	// update displayed text if the project is frontmost, the Notes window is visible and notes for project extra are displayed
	if (g_prjNotes.Get(project) == g_prjNotes.Get(NULL))
	{
		if (NotesWnd* w = g_notesWndMgr.Get()) {
			if (w->IsWndVisible() && g_notesType == SNM_NOTES_PROJECT_EXTRA) {
				w->SetText(buf);
			}
		}
	}
	return;
}
