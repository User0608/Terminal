/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "screenInfo.hpp"

#include "conv.h"
#include "cursor.h"
#include "dbcs.h"
#include "globals.h"
#include "handle.h"
#include "output.h"
#include "stream.h"
#include "tracing.hpp"
#include "utils.hpp"
#include "window.hpp"
#include <math.h>

#pragma hdrstop

#pragma region Construct/Destruct

SCREEN_INFORMATION::SCREEN_INFORMATION(_In_ const CHAR_INFO ciFill, _In_ const CHAR_INFO ciPopupFill) :
    OutputMode(ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT),
    ResizingWindow(0),
    Next(nullptr),
    WheelDelta(0),
    HWheelDelta(0),
    FillOutDbcsLeadChar(0),
    ConvScreenInfo(nullptr),
    ScrollScale(1ul),
    _pConApi(nullptr),
    _pAdapter(nullptr),
    _pStateMachine(nullptr),
    _ptsTabs(nullptr)
{
    this->WriteConsoleDbcsLeadByte[0] = 0;
    this->TextInfo = nullptr;
    this->_srScrollMargins = {0};
    _Attributes = TextAttribute(ciFill.Attributes);
    _PopupAttributes = TextAttribute(ciPopupFill.Attributes);
    if (g_ciConsoleInformation.GetVirtTermLevel() != 0)
    {
        OutputMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    }
}

// Routine Description:
// - This routine frees the memory associated with a screen buffer.
// Arguments:
// - ScreenInfo - screen buffer data to free.
// Return Value:
// Note:
// - console handle table lock must be held when calling this routine
SCREEN_INFORMATION::~SCREEN_INFORMATION()
{
    delete this->TextInfo;
    this->_FreeOutputStateMachine();
    this->ClearTabStops();
}

// Routine Description:
// - This routine allocates and initializes the data associated with a screen buffer.
// Arguments:
// - ScreenInformation - the new screen buffer.
// - dwWindowSize - the initial size of screen buffer's window (in rows/columns)
// - nFont - the initial font to generate text with.
// - dwScreenBufferSize - the initial size of the screen buffer (in rows/columns).
// Return Value:
NTSTATUS SCREEN_INFORMATION::CreateInstance(_In_ COORD coordWindowSize,
                                            _In_ const FontInfo* const pfiFont,
                                            _In_ COORD coordScreenBufferSize,
                                            _In_ CHAR_INFO const ciFill,
                                            _In_ CHAR_INFO const ciPopupFill,
                                            _In_ UINT const uiCursorSize,
                                            _Outptr_ PPSCREEN_INFORMATION const ppScreen)
{
    *ppScreen = nullptr;

    NTSTATUS Status = STATUS_SUCCESS;

    PSCREEN_INFORMATION const pScreen = new SCREEN_INFORMATION(ciFill, ciPopupFill);

    Status = NT_TESTNULL(pScreen);
    if (NT_SUCCESS(Status))
    {
        pScreen->SetScreenBufferSize(coordScreenBufferSize);

        pScreen->_srBufferViewport.Left = 0;
        pScreen->_srBufferViewport.Top = 0;
        pScreen->_srBufferViewport.Right = coordWindowSize.X - 1;
        pScreen->_srBufferViewport.Bottom = coordWindowSize.Y - 1;

        pScreen->SetScreenBufferSize(coordScreenBufferSize);

        Status = TEXT_BUFFER_INFO::CreateInstance(pfiFont, coordScreenBufferSize, ciFill, uiCursorSize, &pScreen->TextInfo);

        if (NT_SUCCESS(Status))
        {
            SetLineChar(pScreen);

            Status = pScreen->_InitializeOutputStateMachine();

            if (NT_SUCCESS(Status))
            {
                *ppScreen = pScreen;
            }
        }
    }

    LOG_IF_NTSTATUS_FAILED(Status);
    return Status;
}

void SCREEN_INFORMATION::SetScreenBufferSize(_In_ const COORD coordNewBufferSize)
{
    COORD coordCandidate;
    coordCandidate.X = max(1, coordNewBufferSize.X);
    coordCandidate.Y = max(1, coordNewBufferSize.Y);
    _coordScreenBufferSize = coordCandidate;
}

COORD SCREEN_INFORMATION::GetScreenBufferSize() const
{
    return _coordScreenBufferSize;
}

WriteBuffer* SCREEN_INFORMATION::GetBufferWriter() const
{
    return _pBufferWriter;
}

AdaptDispatch* SCREEN_INFORMATION::GetAdapterDispatch() const
{
    return _pAdapter;
}

StateMachine* SCREEN_INFORMATION::GetStateMachine() const
{
    return _pStateMachine;
}

// Routine Description:
// - This routine inserts the screen buffer pointer into the console's list of screen buffers.
// Arguments:
// - ScreenInfo - Pointer to screen information structure.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
void SCREEN_INFORMATION::s_InsertScreenBuffer(_In_ PSCREEN_INFORMATION pScreenInfo)
{
    ASSERT(g_ciConsoleInformation.IsConsoleLocked());

    pScreenInfo->Next = g_ciConsoleInformation.ScreenBuffers;
    g_ciConsoleInformation.ScreenBuffers = pScreenInfo;
}

// Routine Description:
// - This routine removes the screen buffer pointer from the console's list of screen buffers.
// Arguments:
// - ScreenInfo - Pointer to screen information structure.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
void SCREEN_INFORMATION::s_RemoveScreenBuffer(_In_ SCREEN_INFORMATION* const pScreenInfo)
{
    if (pScreenInfo == g_ciConsoleInformation.ScreenBuffers)
    {
        g_ciConsoleInformation.ScreenBuffers = pScreenInfo->Next;
    }
    else
    {
        PSCREEN_INFORMATION Cur = g_ciConsoleInformation.ScreenBuffers;
        PSCREEN_INFORMATION Prev = Cur;
        while (Cur != nullptr)
        {
            if (pScreenInfo == Cur)
            {
                break;
            }

            Prev = Cur;
            Cur = Cur->Next;
        }

        ASSERT(Cur != nullptr);
        __analysis_assume(Cur != nullptr);
        Prev->Next = Cur->Next;
    }

    if (pScreenInfo == g_ciConsoleInformation.CurrentScreenBuffer &&
        g_ciConsoleInformation.ScreenBuffers != g_ciConsoleInformation.CurrentScreenBuffer)
    {
        if (g_ciConsoleInformation.ScreenBuffers != nullptr)
        {
            SetActiveScreenBuffer(g_ciConsoleInformation.ScreenBuffers);
        }
        else
        {
            g_ciConsoleInformation.CurrentScreenBuffer = nullptr;
        }
    }

    delete pScreenInfo;
}

#pragma endregion

#pragma region Output State Machine

NTSTATUS SCREEN_INFORMATION::_InitializeOutputStateMachine()
{
    ASSERT(_pConApi == nullptr);
    _pConApi = new ConhostInternalGetSet(this, g_ciConsoleInformation.pInputBuffer);
    NTSTATUS status = NT_TESTNULL(_pConApi);

    if (NT_SUCCESS(status))
    {
        ASSERT(_pBufferWriter == nullptr);
        _pBufferWriter = new WriteBuffer(this);
        status = NT_TESTNULL(_pBufferWriter);
    }

    if (NT_SUCCESS(status))
    {
        ASSERT(_pAdapter == nullptr);
        if (!AdaptDispatch::CreateInstance(_pConApi, _pBufferWriter, _Attributes.GetLegacyAttributes(), &_pAdapter))
        {
            status = STATUS_NO_MEMORY;
        }
    }

    if (NT_SUCCESS(status))
    {
        ASSERT(_pStateMachine == nullptr);
        _pStateMachine = new StateMachine(_pAdapter);
        status = NT_TESTNULL(_pStateMachine);
    }

    if (!NT_SUCCESS(status))
    {
        // if any part of initialization failed, free the allocated ones.
        _FreeOutputStateMachine();
    }

    return status;
}

// If we're an alternate buffer, we want to give the GetSet back to our main
void SCREEN_INFORMATION::_FreeOutputStateMachine()
{
    if (this->_psiMainBuffer == nullptr) // If this is a main buffer
    {
        if (_psiAlternateBuffer != nullptr)
        {
            s_RemoveScreenBuffer(_psiAlternateBuffer);
        }
        if (_pStateMachine != nullptr)
        {
            delete _pStateMachine;
        }

        if (_pAdapter != nullptr)
        {
            delete _pAdapter;
        }

        if (_pBufferWriter != nullptr)
        {
            delete _pBufferWriter;
        }

        if (_pConApi != nullptr)
        {
            delete _pConApi;
        }
    }
    else
    {
        _pConApi->SetActiveScreenBuffer(this->_psiMainBuffer);
        _pBufferWriter->SetActiveScreenBuffer(this->_psiMainBuffer);
    }
}
#pragma endregion

#pragma region Get Data

// Routine Description:
// - Retrieves the window handle attached to this screen information.
// - NOTE: that this can be null if we're running headless.
//   If we were explicitly told to not show a window when starting up (srvinit.cpp), then
//   a window and the corresponding input thread won't be created and CONSOLE_NO_WINDOW flag
//   will be set on the global flags.
// - ALSO: Null is perfectly okay! Just don't do whatever you were about to do because it
//   doesn't matter when we have no window.
// Arguments:
// - <none>
// Return Value:
// - The handle to the window using this screen buffer for rendering or nullptr if no window is attached (headless).
_Must_inspect_result_
Window* SCREEN_INFORMATION::_GetWindow() const
{
    return g_ciConsoleInformation.pWindow;
}

BOOL SCREEN_INFORMATION::IsActiveScreenBuffer() const
{
    // the following macro returns TRUE if the given screen buffer is the active screen buffer.

    //#define ACTIVE_SCREEN_BUFFER(SCREEN_INFO) (g_ciConsoleInformation.CurrentScreenBuffer == SCREEN_INFO)
    return (g_ciConsoleInformation.CurrentScreenBuffer == this);
}

// Routine Description:
// - This routine returns data about the screen buffer.
// Arguments:
// - Size - Pointer to location in which to store screen buffer size.
// - CursorPosition - Pointer to location in which to store the cursor position.
// - ScrollPosition - Pointer to location in which to store the scroll position.
// - Attributes - Pointer to location in which to store the default attributes.
// - CurrentWindowSize - Pointer to location in which to store current window size.
// - MaximumWindowSize - Pointer to location in which to store maximum window size.
// Return Value:
NTSTATUS
SCREEN_INFORMATION::GetScreenBufferInformation(_Out_ PCOORD pcoordSize,
                                               _Out_ PCOORD pcoordCursorPosition,
                                               _Out_ PSMALL_RECT psrWindow,
                                               _Out_ PWORD pwAttributes,
                                               _Out_ PCOORD pcoordMaximumWindowSize,
                                               _Out_ PWORD pwPopupAttributes,
                                               _Out_writes_(COLOR_TABLE_SIZE) LPCOLORREF lpColorTable) const
{
    *pcoordSize = GetScreenBufferSize();

    *pcoordCursorPosition = this->TextInfo->GetCursor()->GetPosition();

    *psrWindow = this->_srBufferViewport;

    *pwAttributes = this->_Attributes.GetLegacyAttributes();
    *pwPopupAttributes = this->_PopupAttributes.GetLegacyAttributes();

    // the copy length must be constant for now to keep OACR happy with buffer overruns.
    memmove(lpColorTable, g_ciConsoleInformation.GetColorTable(), COLOR_TABLE_SIZE * sizeof(COLORREF));

    *pcoordMaximumWindowSize = this->GetMaxWindowSizeInCharacters();

    return STATUS_SUCCESS;
}

// Routine Description:
// - Gets the smallest possible client area in characters. Takes the window client area and divides by the active font dimensions.
// Arguments:
// - coordFontSize - The font size to use for calculation if a screen buffer is not yet attached.
// Return Value:
// - COORD containing the width and height representing the minimum character grid that can be rendered in the window.
COORD SCREEN_INFORMATION::GetMinWindowSizeInCharacters(_In_ COORD const coordFontSize /*= { 1, 1 }*/) const
{
    // prepare rectangle
    RECT const rcWindowInPixels = Window::s_GetMinClientRectInPixels();

    // assign the pixel widths and heights to the final output
    COORD coordClientAreaSize;
    coordClientAreaSize.X = (SHORT)RECT_WIDTH(&rcWindowInPixels);
    coordClientAreaSize.Y = (SHORT)RECT_HEIGHT(&rcWindowInPixels);

    // now retrieve the font size and divide the pixel counts into character counts
    COORD coordFont = coordFontSize; // by default, use the size we were given

    // If text info has been set up, instead retrieve it's font size
    if (this->TextInfo != nullptr)
    {
        coordFont = this->GetScreenFontSize();
    }

    coordClientAreaSize.X /= coordFont.X;
    coordClientAreaSize.Y /= coordFont.Y;

    return coordClientAreaSize;
}

// Routine Description:
// - Gets the maximum client area in characters that would fit on the current monitor or given the current buffer size.
//   Takes the monitor work area and divides by the active font dimensions then limits by buffer size.
// Arguments:
// - coordFontSize - The font size to use for calculation if a screen buffer is not yet attached.
// Return Value:
// - COORD containing the width and height representing the largest character grid that can be rendered on the current monitor and/or from the current buffer size.
COORD SCREEN_INFORMATION::GetMaxWindowSizeInCharacters(_In_ COORD const coordFontSize /*= { 1, 1 }*/) const
{
    COORD coordWindowRestrictedSize = GetLargestWindowSizeInCharacters(coordFontSize);

    COORD coordClientAreaSize;

    // If the buffer is smaller than what the max window would allow, then the max client area can only be as big as the
    // buffer we have.
    const COORD coordScreenBufferSize = GetScreenBufferSize();
    coordClientAreaSize.X = min(coordScreenBufferSize.X, coordWindowRestrictedSize.X);
    coordClientAreaSize.Y = min(coordScreenBufferSize.Y, coordWindowRestrictedSize.Y);

    return coordClientAreaSize;
}

// Routine Description:
// - Gets the largest possible client area in characters if the window were stretched as large as it could go.
// - Takes the window client area and divides by the active font dimensions.
// Arguments:
// - coordFontSize - The font size to use for calculation if a screen buffer is not yet attached.
// Return Value:
// - COORD containing the width and height representing the largest character grid that can be rendered on the current monitor with the maximum size window.
COORD SCREEN_INFORMATION::GetLargestWindowSizeInCharacters(_In_ COORD const coordFontSize /*= { 1, 1 }*/) const
{
    RECT const rcClientInPixels = Window::s_GetMaxClientRectInPixels();

    // first assign the pixel widths and heights to the final output
    COORD coordClientAreaSize;
    coordClientAreaSize.X = (SHORT)RECT_WIDTH(&rcClientInPixels);
    coordClientAreaSize.Y = (SHORT)RECT_HEIGHT(&rcClientInPixels);

    // now retrieve the font size and divide the pixel counts into character counts
    COORD coordFont = coordFontSize; // by default, use the size we were given

    // If renderer has been set up, instead retrieve it's font size
    if (g_pRender != nullptr)
    {
        coordFont = GetScreenFontSize();
    }

    coordClientAreaSize.X /= coordFont.X;
    coordClientAreaSize.Y /= coordFont.Y;

    return coordClientAreaSize;
}

COORD SCREEN_INFORMATION::GetScrollBarSizesInCharacters() const
{
    COORD coordFont = GetScreenFontSize();
    COORD coordBarSizes;
    coordBarSizes.X = (g_sVerticalScrollSize / coordFont.X) + ((g_sVerticalScrollSize % coordFont.X) != 0 ? 1 : 0);
    coordBarSizes.Y = (g_sHorizontalScrollSize / coordFont.Y) + ((g_sHorizontalScrollSize % coordFont.Y) != 0 ? 1 : 0);

    return coordBarSizes;
}

void SCREEN_INFORMATION::GetRequiredConsoleSizeInPixels(_Out_ PSIZE const pRequiredSize) const
{
    COORD const coordFontSize = TextInfo->GetCurrentFont()->GetSize();

    // TODO: Assert valid size boundaries
    pRequiredSize->cx = this->GetScreenWindowSizeX() * coordFontSize.X;
    pRequiredSize->cy = this->GetScreenWindowSizeY() * coordFontSize.Y;
}

SHORT SCREEN_INFORMATION::GetScreenWindowSizeX() const
{
    return CalcWindowSizeX(&this->_srBufferViewport);
}

SHORT SCREEN_INFORMATION::GetScreenWindowSizeY() const
{
    return CalcWindowSizeY(&this->_srBufferViewport);
}

COORD SCREEN_INFORMATION::GetScreenFontSize() const
{
    // If we have no renderer, then we don't really need any sort of pixel math. so the "font size" for the scale factor
    // (which is used almost everywhere around the code as * and / calls) should just be 1,1 so those operations will do
    // effectively nothing.
    COORD coordRet = {1,1};
    if (g_pRender != nullptr)
    {
        coordRet = g_pRender->GetFontSize();
    }

    // For sanity's sake, make sure not to leak 0 out as a possible value. These values are used in division operations.
    coordRet.X = max(coordRet.X, 1);
    coordRet.Y = max(coordRet.Y, 1);

    return coordRet;
}

#pragma endregion

#pragma region Set Data

void SCREEN_INFORMATION::RefreshFontWithRenderer()
{
    if (IsActiveScreenBuffer())
    {
        // Hand the handle to our internal structure to the font change trigger in case it updates it based on what's appropriate.
        if (g_pRender != nullptr)
        {
            g_pRender->TriggerFontChange(g_dpi,
                                         TextInfo->GetDesiredFont(),
                                         TextInfo->GetCurrentFont());
        }
    }
}

void SCREEN_INFORMATION::UpdateFont(_In_ const FontInfo* const pfiNewFont)
{
    FontInfoDesired fiDesiredFont(*pfiNewFont);

    TextInfo->SetDesiredFont(&fiDesiredFont);

    RefreshFontWithRenderer();
}

// NOTE: This method was historically used to notify accessibility apps AND
// to aggregate drawing metadata to determine whether or not to use PolyTextOut.
// After the Nov 2015 graphics refactor, the metadata drawing flag calculation is no longer necessary.
// This now only notifies accessibility apps of a change.
void SCREEN_INFORMATION::ResetTextFlags(_In_ short const sStartX, _In_ short const sStartY, _In_ short const sEndX, _In_ short const sEndY)
{
    SHORT RowIndex;
    ROW* Row;
    WCHAR Char;
    UINT CountOfAttr;
    PTEXT_BUFFER_INFO pTextInfo = this->TextInfo;

    // Fire off a winevent to let accessibility apps know what changed.
    if (this->IsActiveScreenBuffer())
    {
        const COORD coordScreenBufferSize = GetScreenBufferSize();
        ASSERT(sEndX < coordScreenBufferSize.X);

        if (sStartX == sEndX && sStartY == sEndY)
        {
            RowIndex = (pTextInfo->GetFirstRowIndex() + sStartY) % coordScreenBufferSize.Y;
            Row = &pTextInfo->Rows[RowIndex];
            Char = Row->CharRow.Chars[sStartX];
            TextAttributeRun* pAttrRun;
            Row->AttrRow.FindAttrIndex(sStartX, &pAttrRun, &CountOfAttr);

            NotifyWinEvent(EVENT_CONSOLE_UPDATE_SIMPLE,
                           g_ciConsoleInformation.hWnd,
                           MAKELONG(sStartX, sStartY),
                           MAKELONG(Char, g_ciConsoleInformation.GenerateLegacyAttributes(pAttrRun->GetAttributes())));
        }
        else
        {
            NotifyWinEvent(EVENT_CONSOLE_UPDATE_REGION, g_ciConsoleInformation.hWnd, MAKELONG(sStartX, sStartY), MAKELONG(sEndX, sEndY));
        }
        g_ciConsoleInformation.pWindow->SignalUia(UIA_Text_TextChangedEventId);
    }
}

#pragma endregion

#pragma region UI/Refresh

VOID SCREEN_INFORMATION::UpdateScrollBars()
{
    if (!this->IsActiveScreenBuffer())
    {
        return;
    }

    if (g_ciConsoleInformation.Flags & CONSOLE_UPDATING_SCROLL_BARS)
    {
        return;
    }

    g_ciConsoleInformation.Flags |= CONSOLE_UPDATING_SCROLL_BARS;

    Window* const pWindow = _GetWindow();
    if (pWindow != nullptr)
    {
        pWindow->PostUpdateScrollBars();
    }
}

VOID SCREEN_INFORMATION::InternalUpdateScrollBars()
{
    SCROLLINFO si;

    g_ciConsoleInformation.Flags &= ~CONSOLE_UPDATING_SCROLL_BARS;
    if (!this->IsActiveScreenBuffer())
    {
        return;
    }

    this->ResizingWindow++;

    // If this isn't the main buffer, make sure we enable both of the scroll bars.
    //   The alt might come through and disable the scroll bars, this is the only way to re-enable it.
    if(!_IsAltBuffer())
    {
        EnableScrollBar(g_ciConsoleInformation.hWnd, SB_BOTH, ESB_ENABLE_BOTH);
    }

    const COORD coordScreenBufferSize = GetScreenBufferSize();
    si.cbSize = sizeof(si);
    si.fMask = (this->_IsAltBuffer()) ? SIF_ALL | SIF_DISABLENOSCROLL : SIF_ALL;
    si.nPage = this->GetScreenWindowSizeY();
    si.nMin = 0;
    si.nMax = coordScreenBufferSize.Y - 1;
    si.nPos = this->_srBufferViewport.Top;
    SetScrollInfo(g_ciConsoleInformation.hWnd, SB_VERT, &si, TRUE);

    si.cbSize = sizeof(si);
    si.fMask = (this->_IsAltBuffer()) ? SIF_ALL | SIF_DISABLENOSCROLL : SIF_ALL;
    si.nPage = this->GetScreenWindowSizeX();
    si.nMin = 0;
    si.nMax = coordScreenBufferSize.X - 1;
    si.nPos = this->_srBufferViewport.Left;
    SetScrollInfo(g_ciConsoleInformation.hWnd, SB_HORZ, &si, TRUE);

    // Fire off an event to let accessibility apps know the layout has changed.
    NotifyWinEvent(EVENT_CONSOLE_LAYOUT, g_ciConsoleInformation.hWnd, 0, 0);

    this->ResizingWindow--;
}

// Routine Description:
// - Modifies the size of the current viewport to match the width/height of the request given.
// - This will act like a resize operation from the bottom right corner of the window.
// Arguments:
// - pcoordSize - Requested viewport width/heights in characters
// Return Value:
// - <none>
void SCREEN_INFORMATION::SetViewportSize(_In_ const COORD* const pcoordSize)
{
    _InternalSetViewportSize(pcoordSize, false, false);
}

NTSTATUS SCREEN_INFORMATION::SetViewportOrigin(_In_ const BOOL fAbsolute, _In_ const COORD coordWindowOrigin)
{
    // calculate window size
    COORD WindowSize;
    WindowSize.X = (SHORT)GetScreenWindowSizeX();
    WindowSize.Y = (SHORT)GetScreenWindowSizeY();

    SMALL_RECT NewWindow;
    // if relative coordinates, figure out absolute coords.
    if (!fAbsolute)
    {
        if (coordWindowOrigin.X == 0 && coordWindowOrigin.Y == 0)
        {
            return STATUS_SUCCESS;
        }
        NewWindow.Left = _srBufferViewport.Left + coordWindowOrigin.X;
        NewWindow.Top = _srBufferViewport.Top + coordWindowOrigin.Y;
    }
    else
    {
        if (coordWindowOrigin.X == _srBufferViewport.Left && coordWindowOrigin.Y == _srBufferViewport.Top)
        {
            return STATUS_SUCCESS;
        }
        NewWindow.Left = coordWindowOrigin.X;
        NewWindow.Top = coordWindowOrigin.Y;
    }
    NewWindow.Right = (SHORT)(NewWindow.Left + WindowSize.X - 1);
    NewWindow.Bottom = (SHORT)(NewWindow.Top + WindowSize.Y - 1);

    // see if new window origin would extend window beyond extent of screen buffer
    const COORD coordScreenBufferSize = GetScreenBufferSize();
    if (NewWindow.Left < 0 ||
        NewWindow.Top < 0 ||
        NewWindow.Right < 0 ||
        NewWindow.Bottom < 0 ||
        NewWindow.Right >= coordScreenBufferSize.X ||
        NewWindow.Bottom >= coordScreenBufferSize.Y)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (IsActiveScreenBuffer())
    {
        Window* const pWindow = _GetWindow();

        if (pWindow != nullptr)
        {
            // Tell the window that it needs to set itself to the new origin if we're the active buffer.
            pWindow->SetViewportOrigin(NewWindow);
        }
    }
    else
    {
        // Otherwise, just store the new position and go on.
        _srBufferViewport = NewWindow;
        Tracing::s_TraceWindowViewport(_srBufferViewport);
    }

    return STATUS_SUCCESS;
}

// Routine Description:
// - This routine updates the size of the rectangle representing the viewport into the text buffer.
// - It is specified in character count within the buffer.
// - It will be corrected to not exceed the limits of the current screen buffer dimensions.
// Arguments:
// - prcNewViewport - Specifies boundaries for the new viewport.
//                    NOTE: A pointer is used here so the updated value (if corrected) is passed back out to the API call (SetConsoleWindowInfo)
//                          This is not documented functionality (http://msdn.microsoft.com/en-us/library/windows/desktop/ms686125(v=vs.85).aspx)
//                          however, it remains this way to preserve compatibility with apps that might be using it.
// Return Value:
NTSTATUS SCREEN_INFORMATION::SetViewportRect(_In_ SMALL_RECT* const prcNewViewport)
{
    // make sure there's something to do
    if (0 == memcmp(&_srBufferViewport, prcNewViewport, sizeof(SMALL_RECT)))
    {
        return STATUS_SUCCESS;
    }

    if (prcNewViewport->Left < 0)
    {
        prcNewViewport->Right -= prcNewViewport->Left;
        prcNewViewport->Left = 0;
    }
    if (prcNewViewport->Top < 0)
    {
        prcNewViewport->Bottom -= prcNewViewport->Top;
        prcNewViewport->Top = 0;
    }

    const COORD coordScreenBufferSize = GetScreenBufferSize();
    if (prcNewViewport->Right >= coordScreenBufferSize.X)
    {
        prcNewViewport->Right = coordScreenBufferSize.X;
    }
    if (prcNewViewport->Bottom >= coordScreenBufferSize.Y)
    {
        prcNewViewport->Bottom = coordScreenBufferSize.Y;
    }

    _srBufferViewport = *prcNewViewport;
    Tracing::s_TraceWindowViewport(_srBufferViewport);

    return STATUS_SUCCESS;
}

BOOL SCREEN_INFORMATION::SendNotifyBeep() const
{
    if (IsActiveScreenBuffer())
    {
        Window* const pWindow = _GetWindow();
        if (pWindow != nullptr)
        {
            return pWindow->SendNotifyBeep();
        }
    }

    return FALSE;
}

BOOL SCREEN_INFORMATION::PostUpdateWindowSize() const
{
    if (IsActiveScreenBuffer())
    {
        Window* const pWindow = _GetWindow();
        if (pWindow != nullptr)
        {
            return pWindow->PostUpdateWindowSize();
        }
    }

    return false;
}

// Routine Description:
// - Modifies the screen buffer and viewport dimensions when the available client area rendering space changes.
// Arguments:
// - prcClientNew - Client rectangle in pixels after this update
// - prcClientOld - Client rectangle in pixels before this update
// Return Value:
// - <none>
void SCREEN_INFORMATION::ProcessResizeWindow(_In_ const RECT* const prcClientNew, _In_ const RECT* const prcClientOld)
{
    if (_IsAltBuffer())
    {
        // Stash away the size of the window, we'll need to do this to the main when we pop back
        //  We set this on the main, so that main->alt(resize)->alt keeps the resize
        _psiMainBuffer->_fAltWindowChanged = true;
        _psiMainBuffer->_rcAltSavedClientNew = *prcClientNew;
        _psiMainBuffer->_rcAltSavedClientOld = *prcClientOld;
    }

    // 1. In some modes, the screen buffer size needs to change on window size, so do that first.
    _AdjustScreenBuffer(prcClientNew);

    // 2. Now calculate how large the new viewport should be
    COORD coordViewportSize;
    _CalculateViewportSize(prcClientNew, &coordViewportSize);

    // 3. And adjust the existing viewport to match the same dimensions. The old/new comparison is to figure out which side the window was resized from.
    _AdjustViewportSize(prcClientNew, prcClientOld, &coordViewportSize);

    // 4. Finally, update the scroll bars.
    UpdateScrollBars();

    ASSERT(_srBufferViewport.Top >= 0);
    ASSERT(_srBufferViewport.Top < _srBufferViewport.Bottom);
    ASSERT(_srBufferViewport.Left < _srBufferViewport.Right);
}

#pragma endregion

#pragma region Support/Calculation

// Routine Description:
// - This helper converts client pixel areas into the number of characters that could fit into the client window.
// - It requires the buffer size to figure out whether it needs to reserve space for the scroll bars (or not).
// Arguments:
// - prcClientNew - Client region of window in pixels
// - coordBufferOld - Size of backing buffer in characters
// - pcoordClientNewCharacters - The maximum number of characters X by Y that can be displayed in the window with the given backing buffer.
// Return Value:
// - S_OK if math was successful. Check with SUCCEEDED/FAILED macro.
HRESULT SCREEN_INFORMATION::_AdjustScreenBufferHelper(_In_ const RECT* const prcClientNew,
                                                   _In_ COORD const coordBufferOld,
                                                   _Out_ COORD* const pcoordClientNewCharacters)
{
    // Get the font size ready.
    COORD const coordFontSize = GetScreenFontSize();

    // We cannot operate if the font size is 0. This shouldn't happen, but stop early if it does.
    RETURN_HR_IF(E_NOT_VALID_STATE, 0 == coordFontSize.X || 0 == coordFontSize.Y);

    // Find out how much client space we have to work with in the new area.
    SIZE sizeClientNewPixels = { 0 };
    sizeClientNewPixels.cx = RECT_WIDTH(prcClientNew);
    sizeClientNewPixels.cy = RECT_HEIGHT(prcClientNew);

    // Subtract out scroll bar space if scroll bars will be necessary.
    bool fIsHorizontalVisible = false;
    bool fIsVerticalVisible = false;
    s_CalculateScrollbarVisibility(prcClientNew, &coordBufferOld, &coordFontSize, &fIsHorizontalVisible, &fIsVerticalVisible);

    if (fIsHorizontalVisible)
    {
        sizeClientNewPixels.cy -= g_sHorizontalScrollSize;
    }
    if (fIsVerticalVisible)
    {
        sizeClientNewPixels.cx -= g_sVerticalScrollSize;
    }

    // Now with the scroll bars removed, calculate how many characters could fit into the new window area.
    pcoordClientNewCharacters->X = (SHORT)(sizeClientNewPixels.cx / coordFontSize.X);
    pcoordClientNewCharacters->Y = (SHORT)(sizeClientNewPixels.cy / coordFontSize.Y);

    return S_OK;
}

// Routine Description:
// - Modifies the size of the backing text buffer when the window changes to support "intuitive" resizing modes by grabbing the window edges.
// - This function will compensate for scroll bars.
// - Buffer size changes will happen internally to this function.
// Arguments:
// - prcClientNew - Client rectangle in pixels after this update
// Return Value:
// - <none>
HRESULT SCREEN_INFORMATION::_AdjustScreenBuffer(_In_ const RECT* const prcClientNew)
{
    // Prepare the buffer sizes.
    // We need the main's size here to maintain the right scrollbar visibility.
    COORD const coordBufferSizeOld = _IsAltBuffer()? _psiMainBuffer->GetScreenBufferSize() : GetScreenBufferSize();
    COORD coordBufferSizeNew = coordBufferSizeOld;

    // First figure out how many characters we could fit into the new window given the old buffer size
    COORD coordClientNewCharacters;

    RETURN_IF_FAILED(_AdjustScreenBufferHelper(prcClientNew, coordBufferSizeOld, &coordClientNewCharacters));

    // If we're in wrap text mode, then we want to be fixed to the window size. So use the character calculation we just got
    // to fix the buffer and window width together.
    if (g_ciConsoleInformation.GetWrapText())
    {
        coordBufferSizeNew.X = coordClientNewCharacters.X;
    }

    // Reanalyze scroll bars in case we fixed the edge together for word wrap.
    // Use the new buffer client size.
    RETURN_IF_FAILED(_AdjustScreenBufferHelper(prcClientNew, coordBufferSizeNew, &coordClientNewCharacters));

    // Now reanalyze the buffer size and grow if we can fit more characters into the window no matter the console mode.
    if (_IsAltBuffer())
    {
        // The alt buffer always wants to be exactly the size of the screen, never more or less.
        // This prevents scrollbars when you increase the alt buffer size, then decrease it.
        coordBufferSizeNew.X = coordClientNewCharacters.X;
        coordBufferSizeNew.Y = coordClientNewCharacters.Y;
    }
    else
    {
        if (coordClientNewCharacters.X > coordBufferSizeNew.X)
        {
            coordBufferSizeNew.X = coordClientNewCharacters.X;
        }
        if (coordClientNewCharacters.Y > coordBufferSizeNew.Y)
        {
            coordBufferSizeNew.Y = coordClientNewCharacters.Y;
        }
    }

    // Only attempt to modify the buffer if something changed. Expensive operation.
    if (coordBufferSizeOld.X != coordBufferSizeNew.X ||
        coordBufferSizeOld.Y != coordBufferSizeNew.Y)
    {
        CommandLine* const pCommandLine = &CommandLine::Instance();

        // TODO: Deleting and redrawing the command line during resizing can cause flickering. See: http://osgvsowi/658439
        // 1. Delete input string if necessary (see menu.c)
        pCommandLine->Hide(FALSE);
        TextInfo->GetCursor()->SetIsVisible(false);

        // 2. Call the resize screen buffer method (expensive) to redimension the backing buffer (and reflow)
        ResizeScreenBuffer(coordBufferSizeNew, FALSE);

        // 3.  Reprint console input string
        pCommandLine->Show();
        TextInfo->GetCursor()->SetIsVisible(true);
    }

    return S_OK;
}

// Routine Description:
// - Calculates what width/height the viewport must have to consume all the available space in the given client area.
// - This compensates for scroll bars and will leave space in the client area for the bars if necessary.
// Arguments:
// - prcClientArea - The client rectangle in pixels of available rendering space.
// - pcoordSize - Filled with the width/height to which the viewport should be set.
// Return Value:
// - <none>
void SCREEN_INFORMATION::_CalculateViewportSize(_In_ const RECT* const prcClientArea, _Out_ COORD* const pcoordSize)
{
    COORD const coordBufferSize = GetScreenBufferSize();
    COORD const coordFontSize = GetScreenFontSize();

    SIZE sizeClientPixels = { 0 };
    sizeClientPixels.cx = RECT_WIDTH(prcClientArea);
    sizeClientPixels.cy = RECT_HEIGHT(prcClientArea);

    bool fIsHorizontalVisible;
    bool fIsVerticalVisible;
    s_CalculateScrollbarVisibility(prcClientArea, &coordBufferSize, &coordFontSize, &fIsHorizontalVisible, &fIsVerticalVisible);

    if (fIsHorizontalVisible)
    {
        sizeClientPixels.cy -= g_sHorizontalScrollSize;
    }
    if (fIsVerticalVisible)
    {
        sizeClientPixels.cx -= g_sVerticalScrollSize;
    }

    pcoordSize->X = (SHORT)(sizeClientPixels.cx / coordFontSize.X);
    pcoordSize->Y = (SHORT)(sizeClientPixels.cy / coordFontSize.Y);
}

// Routine Description:
// - Modifies the size of the current viewport to match the width/height of the request given.
// - Must specify which corner to adjust from. Default to false/false to resize from the bottom right corner.
// Arguments:
// - pcoordSize - Requested viewport width/heights in characters
// - fResizeFromTop - If false, will trim/add to bottom of viewport first. If true, will trim/add to top.
// - fResizeFromBottom - If false, will trim/add to top of viewport first. If true, will trim/add to left.
// Return Value:
// - <none>
void SCREEN_INFORMATION::_InternalSetViewportSize(_In_ const COORD* const pcoordSize, _In_ bool const fResizeFromTop, _In_ bool const fResizeFromLeft)
{
    short const DeltaX = pcoordSize->X - GetScreenWindowSizeX();
    short const DeltaY = pcoordSize->Y - GetScreenWindowSizeY();
    const COORD coordScreenBufferSize = GetScreenBufferSize();

    // Now we need to determine what our new Window size should be. Note that Window here refers to the character/row window.
    if (fResizeFromLeft)
    {
        // we're being horizontally sized from the left border
        const SHORT sLeftProposed = (_srBufferViewport.Left - DeltaX);
        if (sLeftProposed >= 0)
        {
            // there's enough room in the backlog to just expand left
            this->_srBufferViewport.Left -= DeltaX;
        }
        else
        {
            // if we're resizing horizontally, we want to show as much content above as we can, but we can't show more
            // than the left of the window
            this->_srBufferViewport.Left = 0;
            this->_srBufferViewport.Right += (SHORT)abs(sLeftProposed);
        }
    }
    else
    {
        // we're being horizontally sized from the right border
        const SHORT sRightProposed = (this->_srBufferViewport.Right + DeltaX);
        if (sRightProposed <= (this->_coordScreenBufferSize.X - 1))
        {
            this->_srBufferViewport.Right += DeltaX;
        }
        else
        {
            this->_srBufferViewport.Right = (coordScreenBufferSize.X - 1);
            this->_srBufferViewport.Left -= (sRightProposed - (coordScreenBufferSize.X - 1));
        }
    }

    if (fResizeFromTop)
    {
        const SHORT sTopProposed = (_srBufferViewport.Top - DeltaY);
        // we're being vertically sized from the top border
        if (sTopProposed >= 0)
        {
            // Special case: Only modify the top position if we're not on the 0th row of the buffer.
            // If we're on the 0th row, people expect it to stay stuck to the top of the window, not to start collapsing down and hiding the top rows.
            if (this->_srBufferViewport.Top > 0)
            {
                // there's enough room in the backlog to just expand the top
                this->_srBufferViewport.Top -= DeltaY;
            }
            else
            {
                // If we didn't adjust the top, we need to trim off the number of rows from the bottom instead.
                // NOTE: It's += because DeltaY will be negative already for this circumstance.
                ASSERT(DeltaY <= 0);
                this->_srBufferViewport.Bottom += DeltaY;
            }
        }
        else
        {
            // if we're resizing vertically, we want to show as much content above as we can, but we can't show more
            // than the top of the window
            this->_srBufferViewport.Top = 0;
            this->_srBufferViewport.Bottom += (SHORT)abs(sTopProposed);
        }
    }
    else
    {
        // we're being vertically sized from the bottom border
        const SHORT sBottomProposed = (_srBufferViewport.Bottom + DeltaY);
        if (sBottomProposed <= (coordScreenBufferSize.Y - 1))
        {
            // If the new bottom is supposed to be before the final line of the buffer
            // Check to ensure that we don't hide the prompt by collapsing the window.

            // The final valid end position will be the coordinates of the last character displayed
            // (including any characters in the input line)
            COORD coordValidEnd;
            Selection::Instance().GetValidAreaBoundaries(nullptr, &coordValidEnd);

            // If the bottom of the window when adjusted would be above the final line of valid text...
            if (this->_srBufferViewport.Bottom + DeltaY < coordValidEnd.Y)
            {
                // Adjust the top of the window instead of the bottom (so the lines slide upward)
                this->_srBufferViewport.Top -= DeltaY;

                // If we happened to move the top of the window past the 0th row (first row in the buffer)
                if (this->_srBufferViewport.Top < 0)
                {
                    // Find the amount we went past 0, correct the top of the window back to 0,
                    // and instead adjust the bottom even though it will cause us to lose the prompt line.
                    const short cRemainder = 0 - this->_srBufferViewport.Top;
                    _srBufferViewport.Top += cRemainder;
                    ASSERT(_srBufferViewport.Top == 0);
                    _srBufferViewport.Bottom += cRemainder;
                }
            }
            else
            {
                this->_srBufferViewport.Bottom += DeltaY;
            }
        }
        else
        {
            this->_srBufferViewport.Bottom = (coordScreenBufferSize.Y - 1);
            this->_srBufferViewport.Top -= (sBottomProposed - (coordScreenBufferSize.Y - 1));
        }
    }

    // Ensure the viewport is valid.
    // We can't have a negative left or top.
    if (_srBufferViewport.Left < 0)
    {
        _srBufferViewport.Right -= _srBufferViewport.Left;
        _srBufferViewport.Left = 0;
    }

    if (_srBufferViewport.Top < 0)
    {
        _srBufferViewport.Bottom -= _srBufferViewport.Top;
        _srBufferViewport.Top = 0;
    }

    // Bottom and right cannot pass the final characters in the array.
    _srBufferViewport.Right = min(_srBufferViewport.Right, coordScreenBufferSize.X - 1);
    _srBufferViewport.Bottom = min(_srBufferViewport.Bottom, coordScreenBufferSize.Y - 1);

    Tracing::s_TraceWindowViewport(this->_srBufferViewport);
}

// Routine Description:
// - Modifies the size of the current viewport to match the width/height of the request given.
// - Uses the old and new client areas to determine which side the window was resized from.
// Arguments:
// - prcClientNew - Client rectangle in pixels after this update
// - prcClientOld - Client rectangle in pixels before this update
// - pcoordSize - Requested viewport width/heights in characters
// Return Value:
// - <none>
void SCREEN_INFORMATION::_AdjustViewportSize(_In_ const RECT* const prcClientNew, _In_ const RECT* const prcClientOld, _In_ const COORD* const pcoordSize)
{
    // If the left is the only one that changed (and not the right also), then adjust from the left.
    // Otherwise if the right changes or both changed, bias toward leaving the top-left corner in place and resize from the bottom right.
    // --
    // Resizing from the bottom right is more expected by users.
    // Normally only one dimension (or one corner) will change at a time if the user is moving it.
    // However, if the window is being dragged and forced to resize at a monitor boundary, all 4 will change.
    // In this case especially, users expect the top left to stay in place and the bottom right to adapt.
    bool const fResizeFromLeft = prcClientNew->left != prcClientOld->left && prcClientNew->right == prcClientOld->right;
    bool const fResizeFromTop = prcClientNew->top != prcClientOld->top && prcClientNew->bottom == prcClientOld->bottom;

    _InternalSetViewportSize(pcoordSize, fResizeFromLeft, fResizeFromTop);
}

// Routine Description:
// - From a window client area in pixels, a buffer size, and the font size, this will determine
//   whether scroll bars will need to be shown (and consume a portion of the client area) for the
//   given buffer to be rendered.
// Arguments:
// - prcClientArea - Client area in pixels of the available space for rendering
// - pcoordBufferSize - Buffer size in characters
// - pcoordFontSize - Font size in pixels per character
// - pfIsHorizontalVisible - Indicates whether the horizontal scroll bar (consuming vertical space) will need to be visible
// - pfIsVerticalVisible - Indicates whether the vertical scroll bar (consuming horizontal space) will need to be visible
// Return Value:
// - <none>
void SCREEN_INFORMATION::s_CalculateScrollbarVisibility(_In_ const RECT* const prcClientArea,
                                                        _In_ const COORD* const pcoordBufferSize,
                                                        _In_ const COORD* const pcoordFontSize,
                                                        _Out_ bool* const pfIsHorizontalVisible,
                                                        _Out_ bool* const pfIsVerticalVisible)
{
    ASSERT(prcClientArea->left < prcClientArea->right);
    ASSERT(prcClientArea->top < prcClientArea->bottom);
    ASSERT(pcoordBufferSize->X > 0);
    ASSERT(pcoordBufferSize->Y > 0);
    ASSERT(pcoordFontSize->X > 0);
    ASSERT(pcoordFontSize->Y > 0);

    // Start with bars not visible as the initial state of the client area doesn't account for scroll bars.
    *pfIsHorizontalVisible = false;
    *pfIsVerticalVisible = false;

    // Set up the client area in pixels
    SIZE sizeClientPixels = { 0 };
    sizeClientPixels.cx = RECT_WIDTH(prcClientArea);
    sizeClientPixels.cy = RECT_HEIGHT(prcClientArea);

    // Set up the buffer area in pixels by multiplying the size by the font size scale factor
    SIZE sizeBufferPixels = { 0 };
    sizeBufferPixels.cx = pcoordBufferSize->X * pcoordFontSize->X;
    sizeBufferPixels.cy = pcoordBufferSize->Y * pcoordFontSize->Y;

    // Now figure out whether we need one or both scroll bars.
    // Showing a scroll bar in one direction may necessitate showing the scroll bar in the other (as it will consume client area space).

    if (sizeBufferPixels.cx > sizeClientPixels.cx)
    {
        *pfIsHorizontalVisible = true;

        // If we have a horizontal bar, remove it from available vertical space and check that remaining client area is enough.
        sizeClientPixels.cy -= g_sHorizontalScrollSize;

        if (sizeBufferPixels.cy > sizeClientPixels.cy)
        {
            *pfIsVerticalVisible = true;
        }
    }
    else if (sizeBufferPixels.cy > sizeClientPixels.cy)
    {
        *pfIsVerticalVisible = true;

        // If we have a vertical bar, remove it from available horizontal space and check that remaining client area is enough.
        sizeClientPixels.cx -= g_sVerticalScrollSize;

        if (sizeBufferPixels.cx > sizeClientPixels.cx)
        {
            *pfIsHorizontalVisible = true;
        }
    }
}

bool SCREEN_INFORMATION::IsMaximizedBoth() const
{
    return IsMaximizedX() && IsMaximizedY();
}

bool SCREEN_INFORMATION::IsMaximizedX() const
{
    // If the viewport is displaying the entire size of the allocated buffer, it's maximized.
    return _srBufferViewport.Left == 0 && (_srBufferViewport.Right + 1 == GetScreenBufferSize().X);
}

bool SCREEN_INFORMATION::IsMaximizedY() const
{
    // If the viewport is displaying the entire size of the allocated buffer, it's maximized.
    return _srBufferViewport.Top == 0 && (_srBufferViewport.Bottom + 1 == GetScreenBufferSize().Y);
}

#pragma endregion

// Routine Description:
// - This is a screen resize algorithm which will reflow the ends of lines based on the
//   line wrap state used for clipboard line-based copy.
// Arguments:
// - <in> Coordinates of the new screen size
// Return Value:
// - Success if successful. Invalid parameter if screen buffer size is unexpected. No memory if allocation failed.
NTSTATUS SCREEN_INFORMATION::ResizeWithReflow(_In_ COORD const coordNewScreenSize)
{
    if ((USHORT)coordNewScreenSize.X >= SHORT_MAX || (USHORT)coordNewScreenSize.Y >= SHORT_MAX)
    {
        RIPMSG2(RIP_WARNING, "Invalid screen buffer size (0x%x, 0x%x)", coordNewScreenSize.X, coordNewScreenSize.Y);
        return STATUS_INVALID_PARAMETER;
    }

    // First allocate a new text buffer to take the place of the current one.
    CHAR_INFO ciFill;
    ciFill.Attributes = _Attributes.GetLegacyAttributes();

    TEXT_BUFFER_INFO* pNewBuffer;
    NTSTATUS status = TEXT_BUFFER_INFO::CreateInstance(TextInfo->GetCurrentFont(),
                                                       coordNewScreenSize,
                                                       ciFill,
                                                       0, // temporarily set size to 0 so it won't render.
                                                       &pNewBuffer);

    // Save cursor's relative height versus the viewport
    SHORT const sCursorHeightInViewportBefore = TextInfo->GetCursor()->GetPosition().Y - _srBufferViewport.Top;

    if (NT_SUCCESS(status))
    {
        Cursor* const pOldCursor = TextInfo->GetCursor();
        Cursor* const pNewCursor = pNewBuffer->GetCursor();
        // skip any drawing updates that might occur as we manipulate the new buffer
        pNewCursor->StartDeferDrawing();

        // We need to save the old cursor position so that we can place the new cursor back on the equivalent character
        //   in the new buffer.
        COORD cOldCursorPos = pOldCursor->GetPosition();
        COORD cOldLastChar = TextInfo->GetLastNonSpaceCharacter();

        short const cOldRowsTotal = cOldLastChar.Y + 1;
        short const cOldColsTotal = GetScreenBufferSize().X;

        COORD cNewCursorPos = {0};
        bool fFoundCursorPos = false;
        // Loop through all the rows of the old buffer and reprint them into the new buffer
        for (short iOldRow = 0; iOldRow < cOldRowsTotal; iOldRow++)
        {
            // Fetch the row and its "right" which is the last printable character.
            const ROW* const pRow = TextInfo->GetRowByOffset(iOldRow);
            short iRight = pRow->CharRow.Right;

            // There is a special case here. If the row has a "wrap" flag on it, but the right isn't equal to the width (one index past the final valid index in the row)
            // then there were a bunch trailing of spaces in the row.
            // (But the measuring functions for each row Left/Right do not count spaces as "displayable" so they're not included.)
            // As such, adjust the "right" to be the width of the row to capture all these spaces
            if (pRow->CharRow.WasWrapForced())
            {
                iRight = cOldColsTotal;

                // And a combined special case.
                // If we wrapped off the end of the row by adding a piece of padding because of a double byte LEADING character,
                // then remove one from the "right" to leave this padding out of the copy process.
                if (pRow->CharRow.WasDoubleBytePadded())
                {
                    iRight--;
                }
            }

            // Loop through every character in the current row (up to the "right" boundary, which is one past the final valid character)
            for (short iOldCol = 0; iOldCol < iRight; iOldCol++)
            {
                // Retrieve old character and double-byte attributes
                const WCHAR wchChar = pRow->CharRow.Chars[iOldCol];
                const BYTE bKAttr = pRow->CharRow.KAttrs[iOldCol];

                // Extract the color attribute that applies to this character
                TextAttributeRun* rAttrRun;
                UINT cAttrApplies;

                pRow->AttrRow.FindAttrIndex(iOldCol, &rAttrRun, &cAttrApplies);

                if (iOldCol == cOldCursorPos.X && iOldRow == cOldCursorPos.Y)
                {
                    cNewCursorPos = pNewCursor->GetPosition();
                    fFoundCursorPos = true;
                }

                // Insert it into the new buffer
                if (!pNewBuffer->InsertCharacter(wchChar, bKAttr, rAttrRun->GetAttributes()))
                {
                    status = STATUS_NO_MEMORY;
                    break;
                }
            }
            if (NT_SUCCESS(status))
            {
                // If we didn't have a full row to copy, insert a new line into the new buffer.
                // Only do so if we were not forced to wrap. If we did force a word wrap, then the existing line break was only because we ran out of space.
                if (iRight < cOldColsTotal && !pRow->CharRow.WasWrapForced())
                {
                    if (iRight == cOldCursorPos.X && iOldRow == cOldCursorPos.Y){
                        cNewCursorPos = pNewCursor->GetPosition();
                        fFoundCursorPos = true;
                    }
                    // Only do this if it's not the final line in the buffer.
                    // On the final line, we want the cursor to sit where it is done printing for the cursor adjustment to follow.
                    if (iOldRow < cOldRowsTotal - 1)
                    {
                        status = pNewBuffer->NewlineCursor()? status : STATUS_NO_MEMORY;
                    }
                }
            }
        }
        if (NT_SUCCESS(status))
        {
            // Finish copying remaining parameters from the old text buffer to the new one
            pNewBuffer->CopyProperties(TextInfo);

            // If we found where to put the cursor while placing characters into the buffer,
            //   just put the cursor there. Otherwise we have to advance manually.
            if (fFoundCursorPos)
            {
                pNewCursor->SetPosition(cNewCursorPos);
            }
            else
            {
                // Advance the cursor to the same offset as before
                // get the number of newlines and spaces between the old end of text and the old cursor,
                //   then advance that many newlines and chars
                int iNewlines = cOldCursorPos.Y - cOldLastChar.Y;
                int iIncrements = cOldCursorPos.X - cOldLastChar.X;
                const COORD cNewLastChar = pNewBuffer->GetLastNonSpaceCharacter();

                // If the last row of the new buffer wrapped, there's going to be one less newline needed,
                //   because the cursor is already on the next line
                ROW* pLastRow = pNewBuffer->GetRowByOffset(cNewLastChar.Y);
                if (pLastRow->CharRow.WasWrapForced())
                {
                    iNewlines = max(iNewlines-1, 0);
                }
                else
                {
                    // if this buffer didn't wrap, but the old one DID, then the d(columns) of the
                    //   old buffer will be one more than in this buffer, so new need one LESS.
                    pLastRow = TextInfo->GetRowByOffset(cOldLastChar.Y);
                    if (pLastRow->CharRow.WasWrapForced())
                    {
                        iNewlines = max(iNewlines-1, 0);
                    }
                }

                for (int r = 0; r < iNewlines; r++)
                {
                    if (!pNewBuffer->NewlineCursor())
                    {
                        status = STATUS_NO_MEMORY;
                        break;
                    }
                }
                if (NT_SUCCESS(status))
                {
                    for (int c = 0; c < iIncrements-1; c++)
                    {
                        if (!pNewBuffer->IncrementCursor())
                        {
                            status = STATUS_NO_MEMORY;
                            break;
                        }
                    }
                }
            }
        }
        if (NT_SUCCESS(status))
        {
            // Adjust the viewport so the cursor doesn't wildly fly off up or down.
            SHORT const sCursorHeightInViewportAfter = pNewCursor->GetPosition().Y - _srBufferViewport.Top;
            COORD coordCursorHeightDiff = { 0 };
            coordCursorHeightDiff.Y = sCursorHeightInViewportAfter - sCursorHeightInViewportBefore;
            SetViewportOrigin(FALSE, coordCursorHeightDiff);

            // Save old cursor size before we delete it
            ULONG const ulSize = pOldCursor->GetSize();

            // Free old text buffer
            delete TextInfo;

            // Place new text buffer into position
            TextInfo = pNewBuffer;

            // Set size back to real size as it will be taking over the rendering duties.
            pNewCursor->SetSize(ulSize);
            pNewCursor->EndDeferDrawing();
        }
    }

    return status;
}

//
// Routine Description:
// - This is the legacy screen resize with minimal changes
// Arguments:
// - NewScreenSize - new size of screen.
// Return Value:
// - Success if successful. Invalid parameter if screen buffer size is unexpected. No memory if allocation failed.
NTSTATUS SCREEN_INFORMATION::ResizeTraditional(_In_ COORD const coordNewScreenSize)
{
    SHORT i, j;
    SHORT LimitX, LimitY;
    PWCHAR TextRows, TextRowPtr;
    SHORT TopRow, TopRowIndex;  // new top row of screen buffer
    PBYTE TextRowsA, TextRowPtrA;

    PTEXT_BUFFER_INFO pTextInfo = this->TextInfo;

    if ((USHORT)coordNewScreenSize.X >= 0x7FFF || (USHORT)coordNewScreenSize.Y >= 0x7FFF)
    {
        RIPMSG2(RIP_WARNING, "Invalid screen buffer size (0x%x, 0x%x)", coordNewScreenSize.X, coordNewScreenSize.Y);
        return STATUS_INVALID_PARAMETER;
    }

    TextRows = new WCHAR[coordNewScreenSize.X * coordNewScreenSize.Y];
    if (TextRows == nullptr)
    {
        return STATUS_NO_MEMORY;
    }

    TextRowsA = new BYTE[coordNewScreenSize.X * coordNewScreenSize.Y];
    if (TextRowsA == nullptr)
    {
        delete[] TextRows;
        return STATUS_NO_MEMORY;
    }

    const COORD coordScreenBufferSize = GetScreenBufferSize();
    LimitX = min(coordNewScreenSize.X, coordScreenBufferSize.X);
    LimitY = min(coordNewScreenSize.Y, coordScreenBufferSize.Y);
    TopRow = 0;
    if (coordNewScreenSize.Y <= pTextInfo->GetCursor()->GetPosition().Y)
    {
        TopRow += pTextInfo->GetCursor()->GetPosition().Y - coordNewScreenSize.Y + 1;
    }
    TopRowIndex = (pTextInfo->GetFirstRowIndex() + TopRow) % coordScreenBufferSize.Y;
    if (coordNewScreenSize.Y != coordScreenBufferSize.Y)
    {
        ROW* Temp;
        SHORT NumToCopy, NumToCopy2;

        // resize ROWs array.  first alloc a new ROWs array. then copy the old
        // one over, resetting the FirstRow.
        Temp = new ROW[coordNewScreenSize.Y];
        if (Temp == nullptr)
        {
            delete[] TextRows;
            delete[] TextRowsA;
            return STATUS_NO_MEMORY;
        }

        NumToCopy = coordScreenBufferSize.Y - TopRowIndex;
        if (NumToCopy > coordNewScreenSize.Y)
            NumToCopy = coordNewScreenSize.Y;
        memmove(Temp, &pTextInfo->Rows[TopRowIndex], NumToCopy * sizeof(ROW));
        if (TopRowIndex != 0 && NumToCopy != coordNewScreenSize.Y)
        {
            NumToCopy2 = TopRowIndex;
            if (NumToCopy2 > (coordNewScreenSize.Y - NumToCopy))
                NumToCopy2 = coordNewScreenSize.Y - NumToCopy;
            memmove(&Temp[NumToCopy], pTextInfo->Rows, NumToCopy2 * sizeof(ROW));
        }

        // if the new screen buffer has fewer rows than the existing one,
        // free the extra rows.  if the new screen buffer has more rows
        // than the existing one, allocate new rows.
        if (coordNewScreenSize.Y < coordScreenBufferSize.Y)
        {
            pTextInfo->FreeExtraAttributeRows(TopRowIndex, coordScreenBufferSize.Y, coordNewScreenSize.Y);
        }
        else if (coordNewScreenSize.Y > coordScreenBufferSize.Y)
        {
            for (i = coordScreenBufferSize.Y; i < coordNewScreenSize.Y; i++)
            {
                bool fSuccess = Temp[i].AttrRow.Initialize(coordNewScreenSize.X, this->_Attributes);
                if (!fSuccess)
                {
                    delete[] TextRowsA;
                    delete[] TextRows;
                    delete[] Temp;
                    return STATUS_NO_MEMORY;
                }
            }
        }
        pTextInfo->SetFirstRowIndex(0);
        delete[] pTextInfo->Rows;
        pTextInfo->Rows = Temp;
    }

    // Realloc each row.  any horizontal growth results in the last
    // attribute in a row getting extended.
    TextRowPtrA = TextRowsA;
    for (i = 0, TextRowPtr = TextRows; i < LimitY; i++, TextRowPtr += coordNewScreenSize.X)
    {
        memmove(TextRowPtr, pTextInfo->Rows[i].CharRow.Chars, LimitX * sizeof(WCHAR));
        if (TextRowPtrA)
        {
            memmove(TextRowPtrA, pTextInfo->Rows[i].CharRow.KAttrs, LimitX * sizeof(CHAR));
        }

        for (j = coordScreenBufferSize.X; j < coordNewScreenSize.X; j++)
        {
            TextRowPtr[j] = UNICODE_SPACE;
        }

        if (pTextInfo->Rows[i].CharRow.Right > coordNewScreenSize.X)
        {
            pTextInfo->Rows[i].CharRow.Right = coordNewScreenSize.X;
        }
        pTextInfo->Rows[i].CharRow.Chars = TextRowPtr;

        pTextInfo->Rows[i].CharRow.KAttrs = TextRowPtrA;
        if (TextRowPtrA)
        {
            if (coordNewScreenSize.X > coordScreenBufferSize.X)
                ZeroMemory(TextRowPtrA + coordScreenBufferSize.X, coordNewScreenSize.X - coordScreenBufferSize.X);
            TextRowPtrA += coordNewScreenSize.X;
        }

        pTextInfo->Rows[i].sRowId = i;
    }

    for (; i < coordNewScreenSize.Y; i++, TextRowPtr += coordNewScreenSize.X)
    {
        for (j = 0; j < coordNewScreenSize.X; j++)
        {
            #pragma prefast(suppress:26019, "J must be in the bounds of the X*Y based on the screen buffer size.")
            TextRowPtr[j] = UNICODE_SPACE;
        }

        if (TextRowPtrA)
        {
            ZeroMemory(TextRowPtrA, coordNewScreenSize.X);
        }

        pTextInfo->Rows[i].CharRow.Chars = TextRowPtr;
        pTextInfo->Rows[i].CharRow.Left = coordNewScreenSize.X;
        pTextInfo->Rows[i].CharRow.Right = 0;

        pTextInfo->Rows[i].CharRow.KAttrs = TextRowPtrA;
        if (TextRowPtrA)
        {
            TextRowPtrA += coordNewScreenSize.X;
        }

        pTextInfo->Rows[i].sRowId = i;
    }

    delete[] pTextInfo->TextRows;
    pTextInfo->TextRows = TextRows;
    delete[] pTextInfo->KAttrRows;
    pTextInfo->KAttrRows = TextRowsA;
    pTextInfo->SetCoordBufferSize(coordNewScreenSize);

    NTSTATUS Status = STATUS_SUCCESS;

    if (coordNewScreenSize.X != coordScreenBufferSize.X)
    {
        for (i = 0; i < LimitY; i++)
        {
            ROW* pRow = &pTextInfo->Rows[i];
            bool fSuccess = pRow->AttrRow.Resize(coordScreenBufferSize.X, coordNewScreenSize.X);
            if (!fSuccess)
            {
                Status = STATUS_NO_MEMORY;
                break;
            }
        }
    }

    return Status;
}

//
// Routine Description:
// - This routine resizes the screen buffer.
// Arguments:
// - NewScreenSize - new size of screen.
// - DoScrollBarUpdate - indicates whether to update scroll bars at the end
// Return Value:
// - Success if successful. Invalid parameter if screen buffer size is unexpected. No memory if allocation failed.
NTSTATUS SCREEN_INFORMATION::ResizeScreenBuffer(_In_ const COORD coordNewScreenSize, _In_ const bool fDoScrollBarUpdate)
{
    NTSTATUS status = STATUS_SUCCESS;

    // cancel any active selection before resizing or it will not necessarily line up with the new buffer positions
    Selection::Instance().ClearSelection();

    const bool fWrapText = g_ciConsoleInformation.GetWrapText();
    if (fWrapText)
    {
        status = ResizeWithReflow(coordNewScreenSize);
    }
    else
    {
        status = ResizeTraditional(coordNewScreenSize);
    }

    if (NT_SUCCESS(status))
    {
        SetScreenBufferSize(coordNewScreenSize);

        const COORD coordSetScreenBufferSize = GetScreenBufferSize();
        this->TextInfo->SetCoordBufferSize(coordSetScreenBufferSize);

        this->ResetTextFlags(0, 0, (SHORT)(coordSetScreenBufferSize.X - 1), (SHORT)(coordSetScreenBufferSize.Y - 1));

        if ((!this->ConvScreenInfo))
        {
            if (!NT_SUCCESS(ConsoleImeResizeCompStrScreenBuffer(coordNewScreenSize)))
            {
                // If something went wrong, just bail out.
                return STATUS_INVALID_HANDLE;
            }
        }

        // Fire off an event to let accessibility apps know the layout has changed.
        if (this->IsActiveScreenBuffer())
        {
            NotifyWinEvent(EVENT_CONSOLE_LAYOUT, g_ciConsoleInformation.hWnd, 0, 0);
        }

        if (fDoScrollBarUpdate)
        {
            this->UpdateScrollBars();
        }
        ScreenBufferSizeChange(coordSetScreenBufferSize);
    }

    return status;
}

// Routine Description:
// - Given a rectangle containing screen buffer coordinates (character-level positioning, not pixel)
//   This method will trim the rectangle to ensure it is within the buffer.
//   For example, if the rectangle given has a right position of 85, but the current screen buffer
//   is only reaching from 0-79, then the right position will be set to 79.
// Arguments:
// - psrRect - Pointer to rectangle holding data to be trimmed
// Return Value:
// - <none>
void SCREEN_INFORMATION::ClipToScreenBuffer(_Inout_ SMALL_RECT* const psrClip) const
{
    SMALL_RECT srEdges;
    GetScreenEdges(&srEdges);

    psrClip->Left = max(psrClip->Left, srEdges.Left);
    psrClip->Top = max(psrClip->Top, srEdges.Top);
    psrClip->Right = min(psrClip->Right, srEdges.Right);
    psrClip->Bottom = min(psrClip->Bottom, srEdges.Bottom);
}

// Routine Description:
// - Given a coordinate containing screen buffer coordinates (character-level positioning, not pixel)
//   This method will ensure that it is within the buffer.
// Arguments:
// - pcoordClip - Pointer to coordinate holding data to be trimmed
// Return Value:
// - <none>
void SCREEN_INFORMATION::ClipToScreenBuffer(_Inout_ COORD* const pcoordClip) const
{
    SMALL_RECT srEdges;
    GetScreenEdges(&srEdges);

    pcoordClip->X = max(pcoordClip->X, srEdges.Left);
    pcoordClip->Y = max(pcoordClip->Y, srEdges.Top);
    pcoordClip->X = min(pcoordClip->X, srEdges.Right);
    pcoordClip->Y = min(pcoordClip->Y, srEdges.Bottom);
}

// Routine Description:
// - Gets the edges of the screen buffer.
//   "Edges" refers to the inclusive final positions in each direction of the screen buffer area.
//   For example, a line that is 80 characters long will go from positions 0 to 79 in the buffer.
//   In this case, 0 is the left edge and 79 is the right edge. The last inclusive index of these points in the buffer.
// Arguments:
// - psrEdges - Pointer to rectangle to hold the edge data
// Return Value:
// - <none>
void SCREEN_INFORMATION::GetScreenEdges(_Out_ SMALL_RECT* const psrEdges) const
{
    const COORD coordScreenBufferSize = GetScreenBufferSize();
    psrEdges->Left = 0;
    psrEdges->Right = coordScreenBufferSize.X - 1;
    psrEdges->Top = 0;
    psrEdges->Bottom = coordScreenBufferSize.Y - 1;
}

void SCREEN_INFORMATION::MakeCurrentCursorVisible()
{
    this->MakeCursorVisible(TextInfo->GetCursor()->GetPosition());
}

// Routine Description:
// - This routine sets the cursor size and visibility both in the data structures and on the screen.
// Arguments:
// - ScreenInfo - pointer to screen info structure.
// - Size - cursor size
// - Visible - cursor visibility
// Return Value:
// - Status
NTSTATUS SCREEN_INFORMATION::SetCursorInformation(_In_ ULONG const Size, _In_ BOOLEAN const Visible)
{
    PTEXT_BUFFER_INFO const pTextInfo = this->TextInfo;
    Cursor* const pCursor = pTextInfo->GetCursor();

    pCursor->SetSize(Size);
    pCursor->SetIsVisible(Visible);
    return STATUS_SUCCESS;
}

// Routine Description:
// - This routine sets a flag saying whether the cursor should be displayed
//   with it's default size or it should be modified to indicate the
//   insert/overtype mode has changed.
// Arguments:
// - ScreenInfo - pointer to screen info structure.
// - DoubleCursor - should we indicated non-normal mode
// Return Value:
// - Status
NTSTATUS SCREEN_INFORMATION::SetCursorDBMode(_In_ BOOLEAN const DoubleCursor)
{
    PTEXT_BUFFER_INFO const pTextInfo = this->TextInfo;
    Cursor* const pCursor = pTextInfo->GetCursor();

    if ((pCursor->IsDouble() != DoubleCursor))
    {
        pCursor->SetIsDouble(DoubleCursor);
    }
    return STATUS_SUCCESS;
}

// Routine Description:
// - This routine sets the cursor position in the data structures and on the screen.
// Arguments:
// - ScreenInfo - pointer to screen info structure.
// - Position - new position of cursor
// - TurnOn - true if cursor should be left on, false if should be left off
// Return Value:
// - Status
NTSTATUS SCREEN_INFORMATION::SetCursorPosition(_In_ COORD const Position, _In_ BOOL const TurnOn)
{
    PTEXT_BUFFER_INFO const pTextInfo = this->TextInfo;
    Cursor* const pCursor = pTextInfo->GetCursor();

    //
    // Ensure that the cursor position is within the constraints of the screen
    // buffer.
    //
    const COORD coordScreenBufferSize = GetScreenBufferSize();
    if (Position.X >= coordScreenBufferSize.X || Position.Y >= coordScreenBufferSize.Y || Position.X < 0 || Position.Y < 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    pCursor->SetPosition(Position);

    // if we have the focus, adjust the cursor state
    if (g_ciConsoleInformation.Flags & CONSOLE_HAS_FOCUS)
    {
        if (TurnOn)
        {
            pCursor->SetDelay(FALSE);
            pCursor->SetIsOn(TRUE);
        }
        else
        {
            pCursor->SetDelay(TRUE);
        }
        pCursor->SetHasMoved(TRUE);
    }

    return STATUS_SUCCESS;
}

void SCREEN_INFORMATION::MakeCursorVisible(_In_ const COORD CursorPosition)
{
    COORD WindowOrigin;

    if (CursorPosition.X > this->_srBufferViewport.Right)
    {
        WindowOrigin.X = CursorPosition.X - this->_srBufferViewport.Right;
    }
    else if (CursorPosition.X < this->_srBufferViewport.Left)
    {
        WindowOrigin.X = CursorPosition.X - this->_srBufferViewport.Left;
    }
    else
    {
        WindowOrigin.X = 0;
    }

    if (CursorPosition.Y > this->_srBufferViewport.Bottom)
    {
        WindowOrigin.Y = CursorPosition.Y - this->_srBufferViewport.Bottom;
    }
    else if (CursorPosition.Y < this->_srBufferViewport.Top)
    {
        WindowOrigin.Y = CursorPosition.Y - this->_srBufferViewport.Top;
    }
    else
    {
        WindowOrigin.Y = 0;
    }

    if (WindowOrigin.X != 0 || WindowOrigin.Y != 0)
    {
        this->SetViewportOrigin(FALSE, WindowOrigin);
    }
}

void SCREEN_INFORMATION::SetScrollMargins(_In_ const SMALL_RECT* const psrMargins)
{
    _srScrollMargins = *psrMargins;
}

SMALL_RECT SCREEN_INFORMATION::GetScrollMargins() const
{
    return _srScrollMargins;
}

// Routine Description:
// - Retrieves the active buffer of this buffer. If this buffer has an
//     alternate buffer, this is the alternate buffer. Otherwise, it is this buffer.
// Parameters:
// - None
// Return value:
// - a pointer to this buffer's active buffer.
SCREEN_INFORMATION* const SCREEN_INFORMATION::GetActiveBuffer()
{
    if (_psiAlternateBuffer != nullptr)
    {
        return _psiAlternateBuffer;
    }
    return this;
}

// Routine Description:
// - Retrieves the main buffer of this buffer. If this buffer has an
//     alternate buffer, this is the main buffer. Otherwise, it is this buffer's main buffer.
//     The main buffer is not necessarily the active buffer.
// Parameters:
// - None
// Return value:
// - a pointer to this buffer's main buffer.
SCREEN_INFORMATION* const SCREEN_INFORMATION::GetMainBuffer()
{
    if (_psiMainBuffer != nullptr)
    {
        return _psiMainBuffer;
    }
    return this;
}

// Routine Description:
// - Instantiates a new buffer to be used as an alternate buffer. This buffer
//     does not have a driver handle associated with it and shares a state
//     machine with the main buffer it belongs to.
// Parameters:
// - ppsiNewScreenBuffer - a pointer to recieve the newly created buffer.
// Return value:
// - STATUS_SUCCESS if handled successfully. Otherwise, an approriate status code indicating the error.
NTSTATUS SCREEN_INFORMATION::_CreateAltBuffer(_Out_ SCREEN_INFORMATION** const ppsiNewScreenBuffer)
{
    // Create new screen buffer.
    CHAR_INFO Fill;
    Fill.Char.UnicodeChar = UNICODE_SPACE;
    Fill.Attributes = this->_Attributes.GetLegacyAttributes();

    COORD WindowSize;
    WindowSize.X = (SHORT)CalcWindowSizeX(&_srBufferViewport);
    WindowSize.Y = (SHORT)CalcWindowSizeY(&_srBufferViewport);

    const FontInfo* const pfiExistingFont = this->TextInfo->GetCurrentFont();

    NTSTATUS Status = SCREEN_INFORMATION::CreateInstance(WindowSize, pfiExistingFont, WindowSize, Fill, Fill, CURSOR_SMALL_SIZE, ppsiNewScreenBuffer);
    if (NT_SUCCESS(Status))
    {
        s_InsertScreenBuffer(*ppsiNewScreenBuffer);

        // delete the alt buffer's state machine. We don't want it.
        (*ppsiNewScreenBuffer)->_FreeOutputStateMachine(); // this has to be done before we give it a main buffer
        // we'll attach the GetSet, etc once we successfully make this buffer the active buffer.

        // Set up the new buffers references to our current state machine, dispatcher, getset, etc.
        (*ppsiNewScreenBuffer)->_pStateMachine = _pStateMachine;
        (*ppsiNewScreenBuffer)->_pAdapter = _pAdapter;
        (*ppsiNewScreenBuffer)->_pBufferWriter = _pBufferWriter;
        (*ppsiNewScreenBuffer)->_pConApi = _pConApi;
    }
    return Status;
}

// Routine Description:
// - Creates an "alternate" screen buffer for this buffer. In virtual terminals, there exists both a "main"
//     screen buffer and an alternate. ASBSET creates a new alternate, and switches to it. If there is an already
//     existing alternate, it is discarded. This allows applications to retain one HANDLE, and switch which buffer it points to seamlessly.
// Parameters:
// - None
// Return value:
// - STATUS_SUCCESS if handled successfully. Otherwise, an approriate status code indicating the error.
NTSTATUS SCREEN_INFORMATION::UseAlternateScreenBuffer()
{
    SCREEN_INFORMATION* const psiMain = this->GetMainBuffer();
    // If we're in an alt that resized, resize the main before making the new alt
    if (psiMain->_fAltWindowChanged)
    {
        psiMain->ProcessResizeWindow(&(psiMain->_rcAltSavedClientNew), &(psiMain->_rcAltSavedClientOld));
        psiMain->_fAltWindowChanged = false;
    }

    SCREEN_INFORMATION* psiNewAltBuffer;
    NTSTATUS Status = _CreateAltBuffer(&psiNewAltBuffer);
    if (NT_SUCCESS(Status))
    {
        // if this is already an alternate buffer, we want to make the new
        // buffer the alt on our main buffer, not on ourself, because there
        // can only ever be one main and one alternate.
        SCREEN_INFORMATION* const psiOldAltBuffer = psiMain->_psiAlternateBuffer;

        psiNewAltBuffer->_psiMainBuffer = psiMain;
        psiMain->_psiAlternateBuffer = psiNewAltBuffer;

        if (psiOldAltBuffer != nullptr)
        {
            s_RemoveScreenBuffer(psiOldAltBuffer); // this will also delete the old alt buffer
        }
        // hook it up to our state machine, this needs to be done after deleting the old alt buffer,
        // otherwise deleting the old alt buffer will reattach the GetSet to the main buffer.
        _pConApi->SetActiveScreenBuffer(psiNewAltBuffer);
        _pBufferWriter->SetActiveScreenBuffer(psiNewAltBuffer);

        Status = ::SetActiveScreenBuffer(psiNewAltBuffer);

        // Kind of a hack until we have proper signal channels: If the client app wants window size events, send one for
        // the new alt buffer's size (this is so WSL can update the TTY size when the MainSB.viewportWidth <
        // MainSB.bufferWidth (which can happen with wrap text disabled))
        ScreenBufferSizeChange(psiNewAltBuffer->GetScreenBufferSize());

        // Tell the VT MouseInput handler that we're in the Alt buffer now
        g_ciConsoleInformation.terminalMouseInput.UseAlternateScreenBuffer();

    }
    return Status;
}

// Routine Description:
// - Restores the active buffer to be this buffer's main buffer. If this is the main buffer, then nothing happens.
// Parameters:
// - None
// Return value:
// - STATUS_SUCCESS if handled successfully. Otherwise, an approriate status code indicating the error.
NTSTATUS SCREEN_INFORMATION::UseMainScreenBuffer()
{
    NTSTATUS Status = STATUS_SUCCESS;
    SCREEN_INFORMATION* psiMain = this->_psiMainBuffer;
    if (psiMain != nullptr)
    {
        if (psiMain->_fAltWindowChanged)
        {
            psiMain->ProcessResizeWindow(&(psiMain->_rcAltSavedClientNew), &(psiMain->_rcAltSavedClientOld));
            psiMain->_fAltWindowChanged = false;
        }
        Status = ::SetActiveScreenBuffer(psiMain);
        if (NT_SUCCESS(Status))
        {
            psiMain->UpdateScrollBars(); // The alt had disabled scrollbars, re-enable them

            // send a _coordScreenBufferSizeChangeEvent for the new Sb viewport
            ScreenBufferSizeChange(psiMain->GetScreenBufferSize());

            SCREEN_INFORMATION* psiAlt = psiMain->_psiAlternateBuffer;
            psiMain->_psiAlternateBuffer = nullptr;
            s_RemoveScreenBuffer(psiAlt); // this will also delete the alt buffer
            // deleting the alt buffer will give the GetSet back to it's main

            // Tell the VT MouseInput handler that we're in the main buffer now
            g_ciConsoleInformation.terminalMouseInput.UseMainScreenBuffer();
        }
    }
    return Status;
}

// Routine Description:
// - Helper indicating if the buffer has a main buffer, meaning that this is an alternate buffer.
// Parameters:
// - None
// Return value:
// - true iff this buffer has a main buffer.
bool SCREEN_INFORMATION::_IsAltBuffer() const
{
    return _psiMainBuffer != nullptr;
}

// Routine Description:
// - Sets a VT tab stop in the column sColumn. If there is already a tab there, it does nothing.
// Parameters:
// - sColumn: the column to add a tab stop to.
// Return value:
// - STATUS_SUCCESS if handled successfully. Otherwise, an approriate status code indicating the error.
//      (This is most likely an allocation failure on the instantiation of the new tab stop.)
// Note:
//  This screen buffer is responsible for the lifetime of any tab stops added to it. They can all be freed with ClearTabStops()
NTSTATUS SCREEN_INFORMATION::AddTabStop(_In_ const SHORT sColumn)
{
    NTSTATUS Status = STATUS_NO_MEMORY;

    TabStop* prev = _ptsTabs;
    if (prev == nullptr || prev->sColumn > sColumn) //if there is no head, or we should insert at the head
    {
        _ptsTabs = new TabStop();
        Status = NT_TESTNULL(_ptsTabs);
        if (NT_SUCCESS(Status))
        {
            _ptsTabs->sColumn = sColumn;
            _ptsTabs->ptsNext = prev;
        }
    }
    else
    {
        bool fSearching = true;
        while (fSearching)
        {
            // if there's already a tabstop here, don't add another
            if (prev->sColumn == sColumn)
            {
                break;
            }
            // if we're at the end, or we should insert after prev
            if (prev->ptsNext == nullptr || prev->ptsNext->sColumn > sColumn)
            {
                fSearching = false;
                break;
            }
            else
            {
                prev = prev->ptsNext;
            }
        }
        if (!fSearching) //we broke out by finding the right spot to insert,
        {               // NOT by finding an existing tabstop here.
            TabStop* ptsNewTabStop = new TabStop();
            Status = NT_TESTNULL(ptsNewTabStop);
            if (NT_SUCCESS(Status))
            {
                ptsNewTabStop->sColumn = sColumn;
                ptsNewTabStop->ptsNext = prev->ptsNext;
                prev->ptsNext = ptsNewTabStop;
            }
        }

    }
    return Status;

}

// Routine Description:
// - Clears all of the VT tabs that have been set. This also deletes them.
// Parameters:
// <none>
// Return value:
// <none>
void SCREEN_INFORMATION::ClearTabStops()
{
    TabStop* curr = _ptsTabs;
    while (curr != nullptr)
    {
        TabStop* prev = curr;
        curr = curr->ptsNext;
        delete prev;
    }
    _ptsTabs = nullptr;
}

// Routine Description:
// - Clears the VT tab in the column sColumn (if one has been set). Also deletes it from the heap.
// Parameters:
// - sColumn - The column to clear the tab stop for.
// Return value:
// <none>
void SCREEN_INFORMATION::ClearTabStop(_In_ const SHORT sColumn)
{
    if (AreTabsSet())
    {
        // When we start, there is no previous item.
        TabStop* prev = nullptr;
        // Take the starting "current" item from the class storage.
        TabStop* curr = _ptsTabs;

        // Dig through every element in the list
        while (curr != nullptr)
        {
            // Hold pointer to next item only after we're sure that the current is valid.
            TabStop* next = curr->ptsNext; // Will be nullptr if it's the end of the list. This is OK.

            // If the current one matches the column, take it out of the linked list and join the prev and curr items together.
            if (curr->sColumn == sColumn)
            {
                if (prev == nullptr)
                {
                    // If we had no previous, we're at the list head and need to update the stored list head pointer to the following item.
                    _ptsTabs = next;
                }
                else
                {
                    // If we had a previous, then just connect the next to the "next" pointer of the previous.
                    prev->ptsNext = next;
                }

                // And delete the allocation for the current now that nothing is pointing to it anymore.
                delete curr;

                // Walk forward to the next item.
                // Prev remains the same as we deleted what would have moved there.
                curr = next;
            }
            else
            {
                // Walk forward to the next item.
                prev = curr;
                curr = next;
            }
        }
    }
}

// Routine Description:
// - Places the location that a forwards tab would take cCurrCursorPos to into pcNewCursorPos
// Parameters:
// - cCurrCursorPos - The initial cursor location
// - pcNewCursorPos - The cursor location after a forwards tab.
// Return value:
// - <none>
COORD SCREEN_INFORMATION::GetForwardTab(_In_ const COORD cCurrCursorPos)
{
    COORD cNewCursorPos = cCurrCursorPos;
    SHORT sWidth = GetScreenBufferSize().X - 1;
    if (cCurrCursorPos.X == sWidth)
    {
        cNewCursorPos.X = 0;
        cNewCursorPos.Y += 1;
    }
    else
    {
        TabStop* ptsNext = _ptsTabs;
        while (ptsNext != nullptr)
        {
            if (cCurrCursorPos.X >= ptsNext->sColumn)
            {
                ptsNext = ptsNext->ptsNext;
            }
            else
            {
                break;
            }
        }
        if (ptsNext == nullptr)
        {
            cNewCursorPos.X = sWidth;
        }
        else
        {
            cNewCursorPos.X = ptsNext->sColumn;
        }
    }
    return cNewCursorPos;
}

// Routine Description:
// - Places the location that a backwards tab would take cCurrCursorPos to into pcNewCursorPos
// Parameters:
// - cCurrCursorPos - The initial cursor location
// - pcNewCursorPos - The cursor location after a reverse tab.
// Return value:
// - <none>
COORD SCREEN_INFORMATION::GetReverseTab(_In_ const COORD cCurrCursorPos)
{
    COORD cNewCursorPos = cCurrCursorPos;
    // if we're at 0, or there are NO tabs, or the first tab is farther than where we are
    if (cCurrCursorPos.X == 0 || (_ptsTabs == nullptr) || (_ptsTabs->sColumn >= cCurrCursorPos.X))
    {
        cNewCursorPos.X = 0;
    }
    else // this->_ptsTabs != null, and we're past the first tab stop
    {
        TabStop* ptsCurr;
        // while we still have at least one to iterate over, and we are still farther than the current tabstop
        for (ptsCurr = _ptsTabs;
             ptsCurr->ptsNext != nullptr && cCurrCursorPos.X > ptsCurr->ptsNext->sColumn;
             ptsCurr = ptsCurr->ptsNext)
        {
            ; //just iterate through till the loop is done.
        }

        cNewCursorPos.X = ptsCurr->sColumn;
    }
    return cNewCursorPos;
}

// Routine Description:
// - Returns true if any VT-style tab stops have been set (with AddTabStop)
// Parameters:
// <none>
// Return value:
// - true if any VT-style tab stops have been set
bool SCREEN_INFORMATION::AreTabsSet()
{
    return _ptsTabs != nullptr;
}

// Routine Description:
// - Returns the value of the attributes
// Parameters:
// <none>
// Return value:
// - This screen buffer's attributes
TextAttribute SCREEN_INFORMATION::GetAttributes() const
{
    return _Attributes;
}

// Routine Description:
// - Returns the value of the popup attributes
// Parameters:
// <none>
// Return value:
// - This screen buffer's popup attributes
const TextAttribute* const  SCREEN_INFORMATION::GetPopupAttributes() const
{
    return &_PopupAttributes;
}

// Routine Description:
// - Sets the value of the attributes on this screen buffer. Also propagates
//     the change down to the fill of the text buffer attached to this screen buffer.
// Parameters:
// - attributes - The new value of the attributes to use.
// Return value:
// <none>
void SCREEN_INFORMATION::SetAttributes(_In_ const TextAttribute attributes)
{
    _Attributes.SetFrom(attributes);

    CHAR_INFO ciFill = TextInfo->GetFill();
    ciFill.Attributes = _Attributes.GetLegacyAttributes();
    TextInfo->SetFill(ciFill);
}

// Routine Description:
// - Sets the value of the popup attributes on this screen buffer.
// Parameters:
// - wPopupAttributes - The new value of the popup attributes to use.
// Return value:
// <none>
void SCREEN_INFORMATION::SetPopupAttributes(_In_ const TextAttribute* const pPopupAttributes)
{
    _PopupAttributes = *pPopupAttributes;
}

SMALL_RECT SCREEN_INFORMATION::GetBufferViewport() const
{
    return _srBufferViewport;
}

void SCREEN_INFORMATION::SetBufferViewport(SMALL_RECT srBufferViewport)
{
    _srBufferViewport = srBufferViewport;
}
