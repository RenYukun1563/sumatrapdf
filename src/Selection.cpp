/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include "utils/HttpUtil.h"
#include "utils/JsonParser.h"
#include "utils/ScopedWin.h"
#include "utils/ThreadUtil.h"
#include "utils/Dpi.h"
#include "utils/WinUtil.h"

#include "utils/Log.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GlobalPrefs.h"
#include "ChmModel.h"
#include "DisplayModel.h"
#include "TextSelection.h"
#include "ProgressUpdateUI.h"
#include "Notifications.h"
#include "SumatraConfig.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Selection.h"
#include "Toolbar.h"
#include "Translations.h"
#include "uia/Provider.h"

SelectionOnPage::SelectionOnPage(int pageNo, const RectF* const rect) {
    this->pageNo = pageNo;
    if (rect) {
        this->rect = *rect;
    } else {
        this->rect = RectF();
    }
}

Rect SelectionOnPage::GetRect(DisplayModel* dm) const {
    // if the page is not visible, we return an empty rectangle
    PageInfo* pageInfo = dm->GetPageInfo(pageNo);
    if (!pageInfo || pageInfo->visibleRatio <= 0.0) {
        return Rect();
    }

    return dm->CvtToScreen(pageNo, rect);
}

Vec<SelectionOnPage>* SelectionOnPage::FromRectangle(DisplayModel* dm, Rect rect) {
    Vec<SelectionOnPage>* sel = new Vec<SelectionOnPage>();

    for (int pageNo = dm->GetEngine()->PageCount(); pageNo >= 1; --pageNo) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        ReportIf(!(!pi || 0.0 == pi->visibleRatio || pi->isShown));
        if (!pi || !pi->isShown) {
            continue;
        }

        Rect intersect = rect.Intersect(pi->pageOnScreen);
        if (intersect.IsEmpty()) {
            continue;
        }

        /* selection intersects with a page <pageNo> on the screen */
        RectF isectD = dm->CvtFromScreen(intersect, pageNo);
        sel->Append(SelectionOnPage(pageNo, &isectD));
    }
    sel->Reverse();

    if (sel->size() == 0) {
        delete sel;
        return nullptr;
    }
    return sel;
}

Vec<SelectionOnPage>* SelectionOnPage::FromTextSelect(TextSel* textSel) {
    Vec<SelectionOnPage>* sel = new Vec<SelectionOnPage>(textSel->len);

    for (int i = textSel->len - 1; i >= 0; i--) {
        RectF rect = ToRectF(textSel->rects[i]);
        sel->Append(SelectionOnPage(textSel->pages[i], &rect));
    }
    sel->Reverse();

    if (sel->size() == 0) {
        delete sel;
        return nullptr;
    }
    return sel;
}

void DeleteOldSelectionInfo(MainWindow* win, bool alsoTextSel) {
    win->showSelection = false;
    win->selectionMeasure = SizeF();
    WindowTab* tab = win->CurrentTab();
    if (!tab) {
        return;
    }

    delete tab->selectionOnPage;
    tab->selectionOnPage = nullptr;
    if (alsoTextSel && tab->AsFixed()) {
        tab->AsFixed()->textSelection->Reset();
    }
}

void PaintTransparentRectangles(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF selectionColor, u8 alpha,
                                int margin) {
    // create path from rectangles
    Gdiplus::GraphicsPath path(Gdiplus::FillModeWinding);
    screenRc.Inflate(margin, margin);
    for (size_t i = 0; i < rects.size(); i++) {
        Rect rc = rects.at(i).Intersect(screenRc);
        if (!rc.IsEmpty()) {
            path.AddRectangle(ToGdipRect(rc));
        }
    }

    // fill path (and draw optional outline margin)
    Gdiplus::Graphics gs(hdc);
    u8 r, g, b;
    UnpackColor(selectionColor, r, g, b);
    Gdiplus::Color c(alpha, r, g, b);
    Gdiplus::SolidBrush tmpBrush(c);
    gs.FillPath(&tmpBrush, &path);
    if (margin) {
        path.Outline(nullptr, 0.2f);
        Gdiplus::Pen tmpPen(Gdiplus::Color(alpha, 0, 0, 0), (float)margin);
        gs.DrawPath(&tmpPen, &path);
    }
}

void PaintSelection(MainWindow* win, HDC hdc) {
    ReportIf(!win->AsFixed());

    Vec<Rect> rects;

    if (win->mouseAction == MouseAction::Selecting) {
        // during rectangle selection
        Rect selRect = win->selectionRect;
        if (selRect.dx < 0) {
            selRect.x += selRect.dx;
            selRect.dx *= -1;
        }
        if (selRect.dy < 0) {
            selRect.y += selRect.dy;
            selRect.dy *= -1;
        }

        rects.Append(selRect);
    } else {
        // during text selection or after selection is done
        if (MouseAction::SelectingText == win->mouseAction) {
            UpdateTextSelection(win);
            if (!win->CurrentTab()->selectionOnPage) {
                // prevent the selection from disappearing while the
                // user is still at it (OnSelectionStop removes it
                // if it is still empty at the end)
                win->CurrentTab()->selectionOnPage = new Vec<SelectionOnPage>();
                win->showSelection = true;
            }
        }

        ReportDebugIf(!win->CurrentTab()->selectionOnPage);
        if (!win->CurrentTab()->selectionOnPage) {
            return;
        }

        for (SelectionOnPage& sel : *win->CurrentTab()->selectionOnPage) {
            rects.Append(sel.GetRect(win->AsFixed()));
        }
    }

    ParsedColor* parsedCol = GetPrefsColor(gGlobalPrefs->fixedPageUI.selectionColor);
    PaintTransparentRectangles(hdc, win->canvasRc, rects, parsedCol->col);
}

void UpdateTextSelection(MainWindow* win, bool select) {
    if (!win->AsFixed()) {
        return;
    }

    // logf("UpdateTextSelection: select: %d\n", (int)select);
    DisplayModel* dm = win->AsFixed();
    if (select) {
        int pageNo = dm->GetPageNoByPoint(win->selectionRect.BR());
        if (win->ctrl->ValidPageNo(pageNo)) {
            PointF pt = dm->CvtFromScreen(win->selectionRect.BR(), pageNo);
            dm->textSelection->SelectUpTo(pageNo, pt.x, pt.y);
        }
    }

    DeleteOldSelectionInfo(win);
    win->CurrentTab()->selectionOnPage = SelectionOnPage::FromTextSelect(&dm->textSelection->result);
    win->showSelection = win->CurrentTab()->selectionOnPage != nullptr;

    if (win->uiaProvider) {
        win->uiaProvider->OnSelectionChanged();
    }
    ToolbarUpdateStateForWindow(win, false);
}

// isTextSelectionOut is set to true if this is text-only selection (as opposed to
// rectangular selection)
// caller needs to str::Free() the result
TempStr GetSelectedTextTemp(WindowTab* tab, const char* lineSep, bool& isTextOnlySelectionOut) {
    if (!tab || !tab->selectionOnPage) {
        return nullptr;
    }
    if (tab->selectionOnPage->size() == 0) {
        return nullptr;
    }
    DisplayModel* dm = tab->AsFixed();
    ReportIf(!dm);
    if (!dm) {
        return nullptr;
    }
    if (dm->GetEngine()->IsImageCollection()) {
        return nullptr;
    }

    isTextOnlySelectionOut = dm->textSelection->result.len > 0;
    if (isTextOnlySelectionOut) {
        WCHAR* s = dm->textSelection->ExtractText(lineSep);
        TempStr res = ToUtf8Temp(s);
        str::Free(s);
        return res;
    }
    StrVec selections;
    for (SelectionOnPage& sel : *tab->selectionOnPage) {
        // selection may reference pages that no longer exist after a reload
        if (!dm->ValidPageNo(sel.pageNo)) {
            continue;
        }
        char* text = dm->GetTextInRegion(sel.pageNo, sel.rect);
        if (!str::IsEmpty(text)) {
            selections.Append(text);
        }
        str::Free(text);
    }
    if (selections.Size() == 0) {
        return nullptr;
    }
    TempStr s = JoinTemp(&selections, lineSep);
    return s;
}

static volatile LONG gSelectionTranslationSerial = 0;
static constexpr Kind kNotifSelectionTranslation = "selectionTranslation";
static constexpr const char* kSelectionTranslationPrompt =
    "You are a translation engine. Translate the user's text to ${targetlang}. Return only the translation.";
static constexpr const char* kTargetLangStr = "${targetlang}";
static constexpr const char* kSelectionStr = "${selection}";

static bool IsLatestSelectionTranslation(int requestId) {
    LONG latest = InterlockedOr(&gSelectionTranslationSerial, 0);
    return requestId == (int)latest;
}

static void ShowSelectionTranslationNotification(HWND hwndParent, const char* msg, bool warning, int timeoutMs) {
    if (!hwndParent || !IsWindow(hwndParent)) {
        return;
    }

    NotificationWnd* wnd = GetNotificationForGroup(hwndParent, kNotifSelectionTranslation);
    if (wnd) {
        NotificationUpdateMessage(wnd, msg, timeoutMs, warning);
        return;
    }

    NotificationCreateArgs args;
    args.hwndParent = hwndParent;
    args.groupId = kNotifSelectionTranslation;
    args.warning = warning;
    args.timeoutMs = timeoutMs;
    args.shrinkLimit = 0.8f;
    args.msg = msg;
    ShowNotification(args);
}

static void AppendJsonString(StrBuilder& dst, const char* s) {
    dst.AppendChar('"');
    if (s) {
        for (const char* p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            switch (c) {
                case '"':
                    dst.Append("\\\"");
                    break;
                case '\\':
                    dst.Append("\\\\");
                    break;
                case '\b':
                    dst.Append("\\b");
                    break;
                case '\f':
                    dst.Append("\\f");
                    break;
                case '\n':
                    dst.Append("\\n");
                    break;
                case '\r':
                    dst.Append("\\r");
                    break;
                case '\t':
                    dst.Append("\\t");
                    break;
                default:
                    if (c < 0x20) {
                        dst.AppendFmt("\\u%04x", (int)c);
                    } else {
                        dst.AppendChar((char)c);
                    }
                    break;
            }
        }
    }
    dst.AppendChar('"');
}

static const char* SkipJsonWs(const char* s) {
    while (s && str::IsWs(*s)) {
        s++;
    }
    return s;
}

static char* DupTrimmed(const char* s) {
    if (!s) {
        return nullptr;
    }
    char* res = str::Dup(s);
    str::TrimWSInPlace(res, str::TrimOpt::Both);
    if (str::IsEmpty(res)) {
        str::Free(res);
        return nullptr;
    }
    return res;
}

struct TranslationJsonVisitor : json::ValueVisitor {
    AutoFreeStr content;
    AutoFreeStr error;

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (type != json::Type::String) {
            return true;
        }
        if (str::Eq(path, "/choices[0]/message/content") || str::Eq(path, "/choices[0]/text") ||
            str::Eq(path, "/output_text") || str::Eq(path, "/output[0]/content[0]/text")) {
            content.SetCopy(value);
            return false;
        }
        if (str::Eq(path, "/error/message")) {
            error.SetCopy(value);
        }
        return true;
    }
};

static char* ExtractTranslationFromResponse(const char* response) {
    const char* s = SkipJsonWs(response);
    if (!s || !*s) {
        return nullptr;
    }
    if (*s != '{') {
        return DupTrimmed(s);
    }

    TranslationJsonVisitor visitor;
    if (!json::Parse(s, &visitor) || visitor.content.empty()) {
        return nullptr;
    }
    return DupTrimmed(visitor.content.Get());
}

static char* ExtractErrorFromResponse(const char* response) {
    const char* s = SkipJsonWs(response);
    if (!s || *s != '{') {
        return nullptr;
    }

    TranslationJsonVisitor visitor;
    if (!json::Parse(s, &visitor) || visitor.error.empty()) {
        return nullptr;
    }
    return DupTrimmed(visitor.error.Get());
}

static TempStr HeaderValueTemp(const char* s) {
    if (str::IsEmptyOrWhiteSpace(s)) {
        return nullptr;
    }
    TempStr res = str::DupTemp(s);
    str::RemoveCharsInPlace(res, "\r\n");
    return res;
}

struct SelectionTranslationData {
    HWND hwndParent = nullptr;
    int requestId = 0;
    AutoFreeStr endpoint;
    AutoFreeStr apiKey;
    AutoFreeStr model;
    AutoFreeStr targetLang;
    AutoFreeStr systemPrompt;
    AutoFreeStr selectedText;
};

struct SelectionTranslationResult {
    HWND hwndParent = nullptr;
    int requestId = 0;
    bool ok = false;
    AutoFreeStr msg;
};

static TempStr ExpandTranslationPromptTemp(SelectionTranslationData* data) {
    const char* prompt = data->systemPrompt.Get();
    if (str::IsEmptyOrWhiteSpace(prompt)) {
        prompt = kSelectionTranslationPrompt;
    }
    const char* targetLang = data->targetLang.Get();
    if (str::IsEmptyOrWhiteSpace(targetLang)) {
        targetLang = "Chinese";
    }

    TempStr res = str::ReplaceNoCaseTemp(prompt, kTargetLangStr, targetLang);
    res = str::ReplaceNoCaseTemp(res, kSelectionStr, data->selectedText.Get());
    return res;
}

static void BuildTranslationRequestBody(SelectionTranslationData* data, StrBuilder& body) {
    TempStr prompt = ExpandTranslationPromptTemp(data);

    body.Append("{\"model\":");
    AppendJsonString(body, data->model.Get());
    body.Append(",\"messages\":[{\"role\":\"system\",\"content\":");
    AppendJsonString(body, prompt);
    body.Append("},{\"role\":\"user\",\"content\":");
    AppendJsonString(body, data->selectedText.Get());
    body.Append("}],\"temperature\":0}");
}

static bool RunSelectionTranslation(SelectionTranslationData* data, AutoFreeStr& msgOut) {
    StrBuilder headers;
    headers.Append("Content-Type: application/json\r\n");
    TempStr apiKey = HeaderValueTemp(data->apiKey.Get());
    if (apiKey) {
        headers.AppendFmt("Authorization: Bearer %s\r\n", apiKey);
    }

    StrBuilder body(4096);
    BuildTranslationRequestBody(data, body);

    HttpRsp rsp;
    bool ok = HttpPost(data->endpoint.Get(), &headers, &body, &rsp);
    if (ok) {
        char* translated = ExtractTranslationFromResponse(rsp.data.Get());
        if (translated) {
            msgOut = translated;
            return true;
        }
        msgOut = str::Dup(_TRA("Translation response did not contain text."));
        return false;
    }

    AutoFreeStr err = ExtractErrorFromResponse(rsp.data.Get());
    if (!err.empty()) {
        msgOut = str::Format("Translation failed: %s", err.Get());
    } else if (rsp.httpStatusCode != (DWORD)-1) {
        msgOut = str::Format("Translation failed (HTTP %u).", (unsigned)rsp.httpStatusCode);
    } else {
        msgOut = str::Dup(_TRA("Translation request failed."));
    }
    return false;
}

static void FinishSelectionTranslation(SelectionTranslationResult* result) {
    if (!IsLatestSelectionTranslation(result->requestId)) {
        delete result;
        return;
    }

    int timeoutMs = result->ok ? 0 : kNotif5SecsTimeOut;
    ShowSelectionTranslationNotification(result->hwndParent, result->msg.Get(), !result->ok, timeoutMs);
    delete result;
}

static void SelectionTranslationThread(SelectionTranslationData* data) {
    AutoFreeStr msg;
    bool ok = RunSelectionTranslation(data, msg);

    auto result = new SelectionTranslationResult();
    result->hwndParent = data->hwndParent;
    result->requestId = data->requestId;
    result->ok = ok;
    result->msg = msg.Release();

    uitask::Post(MkFunc0<SelectionTranslationResult>(FinishSelectionTranslation, result), "SelectionTranslationFinish");
    delete data;
}

void TranslateSelectionWithLLM(WindowTab* tab, bool automatic) {
    if (!gGlobalPrefs || !tab || !tab->win || !tab->selectionOnPage) {
        return;
    }
    if (automatic && !gGlobalPrefs->selectionTranslation.enabled) {
        return;
    }
    if (!HasPermission(Perm::InternetAccess) || !HasPermission(Perm::CopySelection)) {
        if (!automatic) {
            ShowSelectionTranslationNotification(tab->win->hwndCanvas, _TRA("Selection translation is not allowed."),
                                                 true, kNotif5SecsTimeOut);
        }
        return;
    }

    const char* endpoint = gGlobalPrefs->selectionTranslation.endpoint;
    const char* model = gGlobalPrefs->selectionTranslation.model;
    if (str::IsEmptyOrWhiteSpace(endpoint) || str::IsEmptyOrWhiteSpace(model)) {
        if (!automatic) {
            ShowSelectionTranslationNotification(
                tab->win->hwndCanvas,
                _TRA("Configure SelectionTranslation.Endpoint and SelectionTranslation.Model in Advanced Settings."),
                true, kNotif5SecsTimeOut);
        }
        return;
    }

    bool isTextOnlySelection = false;
    TempStr selText = GetSelectedTextTemp(tab, "\n", isTextOnlySelection);
    if (str::IsEmptyOrWhiteSpace(selText)) {
        return;
    }

    int maxChars = gGlobalPrefs->selectionTranslation.maxChars;
    maxChars = limitValue(maxChars <= 0 ? 4000 : maxChars, 128, 16000);
    TempStr shortened = ShortenStringUtf8Temp(selText, maxChars);
    AutoFreeStr selected = str::Dup(shortened);
    str::TrimWSInPlace(selected.Get(), str::TrimOpt::Both);
    if (selected.empty()) {
        return;
    }

    int requestId = (int)InterlockedIncrement(&gSelectionTranslationSerial);
    auto data = new SelectionTranslationData();
    data->hwndParent = tab->win->hwndCanvas;
    data->requestId = requestId;
    data->endpoint.SetCopy(endpoint);
    data->apiKey.SetCopy(gGlobalPrefs->selectionTranslation.apiKey);
    data->model.SetCopy(model);
    data->targetLang.SetCopy(gGlobalPrefs->selectionTranslation.targetLanguage);
    data->systemPrompt.SetCopy(gGlobalPrefs->selectionTranslation.systemPrompt);
    data->selectedText = selected.Release();

    ShowSelectionTranslationNotification(tab->win->hwndCanvas, _TRA("Translating selection..."), false, 0);
    RunAsync(MkFunc0<SelectionTranslationData>(SelectionTranslationThread, data), "SelectionTranslation");
}

void CopySelectionToClipboard(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    ReportIf(tab->selectionOnPage->size() == 0 && win->mouseAction != MouseAction::SelectingText);

    if (!OpenClipboard(nullptr)) {
        return;
    }
    EmptyClipboard();
    defer {
        CloseClipboard();
    };

    DisplayModel* dm = win->AsFixed();
    TempStr selText = nullptr;
    bool isTextOnlySelectionOut = false;
    if (!gDisableDocumentRestrictions && (dm && !dm->GetEngine()->AllowsCopyingText())) {
        NotificationCreateArgs args;
        args.hwndParent = win->hwndCanvas;
        args.msg = _TRA("Copying text was denied (copying as image only)");
        ShowNotification(args);
    } else {
        selText = GetSelectedTextTemp(tab, "\r\n", isTextOnlySelectionOut);
    }

    if (!str::IsEmpty(selText)) {
        AppendTextToClipboard(selText);
    }

    if (isTextOnlySelectionOut) {
        // don't also copy the first line of a text selection as an image
        return;
    }

    if (!dm || !tab->selectionOnPage || tab->selectionOnPage->size() == 0) {
        return;
    }
    /* also copy a screenshot of the current selection to the clipboard */
    SelectionOnPage* selOnPage = &tab->selectionOnPage->at(0);
    if (!dm->ValidPageNo(selOnPage->pageNo)) {
        return;
    }
    float zoom = dm->GetZoomReal(selOnPage->pageNo);
    int rotation = dm->GetRotation();
    RenderPageArgs args(selOnPage->pageNo, zoom, rotation, &selOnPage->rect, RenderTarget::Export);
    RenderedBitmap* bmp = dm->GetEngine()->RenderPage(args);
    if (bmp) {
        CopyImageToClipboard(bmp->GetBitmap(), true);
    }
    delete bmp;
}

void OnSelectAll(MainWindow* win, bool textOnly) {
    if (!HasPermission(Perm::CopySelection)) {
        return;
    }

    if (HwndIsFocused(win->hwndFindEdit) || HwndIsFocused(win->hwndPageEdit)) {
        EditSelectAll(GetFocus());
        return;
    }

    if (win->AsChm()) {
        win->AsChm()->SelectAll();
        return;
    }
    if (!win->AsFixed()) {
        return;
    }

    DisplayModel* dm = win->AsFixed();
    if (textOnly) {
        int pageNo;
        for (pageNo = 1; !dm->PageShown(pageNo); pageNo++) {
            ;
        }
        dm->textSelection->StartAt(pageNo, 0);
        for (pageNo = win->ctrl->PageCount(); !dm->PageShown(pageNo); pageNo--) {
            ;
        }
        dm->textSelection->SelectUpTo(pageNo, -1);
        win->selectionRect = Rect::FromXY(INT_MIN / 2, INT_MIN / 2, INT_MAX, INT_MAX);
        UpdateTextSelection(win);
    } else {
        DeleteOldSelectionInfo(win, true);
        win->selectionRect = Rect::FromXY(INT_MIN / 2, INT_MIN / 2, INT_MAX, INT_MAX);
        win->CurrentTab()->selectionOnPage = SelectionOnPage::FromRectangle(dm, win->selectionRect);
    }

    win->showSelection = win->CurrentTab()->selectionOnPage != nullptr;
    ScheduleRepaint(win, 0);
}

#define SELECT_AUTOSCROLL_AREA_WIDTH DpiScale(win->hwndFrame, 15)
#define SELECT_AUTOSCROLL_STEP_LENGTH DpiScale(win->hwndFrame, 10)

bool NeedsSelectionEdgeAutoscroll(MainWindow* win, int x, int y) {
    return x < SELECT_AUTOSCROLL_AREA_WIDTH || x > win->canvasRc.dx - SELECT_AUTOSCROLL_AREA_WIDTH ||
           y < SELECT_AUTOSCROLL_AREA_WIDTH || y > win->canvasRc.dy - SELECT_AUTOSCROLL_AREA_WIDTH;
}

void OnSelectionEdgeAutoscroll(MainWindow* win, int x, int y) {
    int dx = 0, dy = 0;

    if (x < SELECT_AUTOSCROLL_AREA_WIDTH) {
        dx = -SELECT_AUTOSCROLL_STEP_LENGTH;
    } else if (x > win->canvasRc.dx - SELECT_AUTOSCROLL_AREA_WIDTH) {
        dx = SELECT_AUTOSCROLL_STEP_LENGTH;
    }
    if (y < SELECT_AUTOSCROLL_AREA_WIDTH) {
        dy = -SELECT_AUTOSCROLL_STEP_LENGTH;
    } else if (y > win->canvasRc.dy - SELECT_AUTOSCROLL_AREA_WIDTH) {
        dy = SELECT_AUTOSCROLL_STEP_LENGTH;
    }

    ReportIf(NeedsSelectionEdgeAutoscroll(win, x, y) != (dx != 0 || dy != 0));
    if (dx != 0 || dy != 0) {
        ReportIf(!win->AsFixed());
        DisplayModel* dm = win->AsFixed();
        Point oldOffset = dm->GetViewPort().TL();
        win->MoveDocBy(dx, dy);

        dx = dm->GetViewPort().x - oldOffset.x;
        dy = dm->GetViewPort().y - oldOffset.y;
        win->selectionRect.x -= dx;
        win->selectionRect.y -= dy;
        win->selectionRect.dx += dx;
        win->selectionRect.dy += dy;
    }
}

void OnSelectionStart(MainWindow* win, int x, int y, WPARAM) {
    ReportIf(!win->AsFixed());
    DeleteOldSelectionInfo(win, true);

    win->selectionRect = Rect(x, y, 0, 0);
    win->showSelection = true;
    win->mouseAction = MouseAction::Selecting;

    bool isShift = IsShiftPressed();
    bool isCtrl = IsCtrlPressed();

    // Ctrl+drag forces a rectangular selection
    if (!isCtrl || isShift) {
        DisplayModel* dm = win->AsFixed();
        int pageNo = dm->GetPageNoByPoint(Point(x, y));
        if (dm->ValidPageNo(pageNo)) {
            PointF pt = dm->CvtFromScreen(Point(x, y), pageNo);
            dm->textSelection->StartAt(pageNo, pt.x, pt.y);
            win->mouseAction = MouseAction::SelectingText;
        }
    }

    SetCapture(win->hwndCanvas);
    SetTimer(win->hwndCanvas, SMOOTHSCROLL_TIMER_ID, SMOOTHSCROLL_DELAY_IN_MS, nullptr);
    ScheduleRepaint(win, 0);
}

void OnSelectionStop(MainWindow* win, int x, int y, bool aborted) {
    if (GetCapture() == win->hwndCanvas) {
        ReleaseCapture();
    }
    KillTimer(win->hwndCanvas, SMOOTHSCROLL_TIMER_ID);

    // update the text selection before changing the selectionRect
    if (MouseAction::SelectingText == win->mouseAction) {
        UpdateTextSelection(win);
    }

    win->selectionRect = Rect::FromXY(win->selectionRect.x, win->selectionRect.y, x, y);
    if (aborted || (MouseAction::Selecting == win->mouseAction ? win->selectionRect.IsEmpty()
                                                               : !win->CurrentTab()->selectionOnPage)) {
        DeleteOldSelectionInfo(win, true);
    } else if (win->mouseAction == MouseAction::Selecting) {
        win->CurrentTab()->selectionOnPage = SelectionOnPage::FromRectangle(win->AsFixed(), win->selectionRect);
        win->showSelection = win->CurrentTab()->selectionOnPage != nullptr;
    }
    ScheduleRepaint(win, 0);
    if (!aborted) {
        TranslateSelectionWithLLM(win->CurrentTab(), true);
    }
}
