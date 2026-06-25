#include "SelectableTextView.h"

#include "external/IconsFontAwesome6.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace doriax::editor {

void SelectableTextView::rebuild(const std::vector<Paragraph>& paragraphs, float wrapWidth) {
    buf.clear();
    lineOffsets.clear();
    lineOffsets.push_back(0);
    lineColors.clear();
    lineFonts.clear();
    lineHardBreak.clear();

    if (wrapWidth <= 0.0f) {
        wrapWidth = 1.0f;
    }

    ImFont* defaultFont = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();

    auto pushLine = [&](const std::string& line, ImU32 color, ImFont* lineFont,
                        bool hardBreak, bool appendNewline) {
        buf.append(line.c_str());
        if (appendNewline) {
            buf.append("\n");
        }
        lineOffsets.push_back(buf.size());
        lineColors.push_back(color);
        lineFonts.push_back(lineFont);
        lineHardBreak.push_back(hardBreak ? 1 : 0);
    };

    for (size_t pi = 0; pi < paragraphs.size(); ++pi) {
        const Paragraph& para = paragraphs[pi];
        const bool lastPara = (pi + 1 == paragraphs.size());
        ImFont* font = para.font ? para.font : defaultFont;

        std::string currentLine;
        float currentWidth = 0.0f;
        int lastBreakPos = 0;       // byte offset in currentLine just past the last space
        float widthAtBreak = 0.0f;  // width of currentLine up to lastBreakPos
        const char* s = para.text.data();
        const char* e = s + para.text.size();

        while (s < e) {
            unsigned int cp = 0;
            int len = ImTextCharFromUtf8(&cp, s, e);
            if (len <= 0) {
                cp = static_cast<unsigned char>(*s);
                len = 1;
            }
            if (cp == '\r') {
                s += len;
                continue;
            }
            if (cp == '\n') {
                pushLine(currentLine, para.color, para.font, true, true);
                currentLine.clear();
                currentWidth = 0.0f;
                lastBreakPos = 0;
                widthAtBreak = 0.0f;
                s += len;
                continue;
            }

            float cw = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, s, s + len).x;
            if (currentWidth + cw > wrapWidth && !currentLine.empty()) {
                if (lastBreakPos > 0 && lastBreakPos < (int)currentLine.size()) {
                    // Break after the last space; carry the trailing partial word
                    // to the next line. The space stays so copies rejoin cleanly.
                    std::string tail = currentLine.substr(lastBreakPos);
                    pushLine(currentLine.substr(0, lastBreakPos), para.color, para.font, false, true);
                    currentLine = tail;
                    currentWidth -= widthAtBreak;
                } else {
                    // A single word longer than the line: fall back to char wrap.
                    pushLine(currentLine, para.color, para.font, false, true);
                    currentLine.clear();
                    currentWidth = 0.0f;
                }
                lastBreakPos = 0;
                widthAtBreak = 0.0f;
            }

            currentLine.append(s, len);
            currentWidth += cw;
            if (cp == ' ' || cp == '\t') {
                lastBreakPos = (int)currentLine.size();
                widthAtBreak = currentWidth;
            }
            s += len;
        }

        // Final line of the paragraph carries a hard break; no trailing newline
        // for the very last paragraph so the buffer never ends on a blank line.
        pushLine(currentLine, para.color, para.font, true, !lastPara);
    }

    if (selectionStart > buf.size() || selectionEnd > buf.size()) {
        selectionStart = selectionEnd = -1;
        hasStoredSelection = false;
        isSelecting = false;
    }
}

int SelectableTextView::clampIndexToCodepointBoundary(int idx) const {
    idx = std::max(0, std::min(idx, (int)buf.size()));
    while (idx > 0) {
        unsigned char c = (unsigned char)buf[idx];
        if ((c & 0xC0) != 0x80) break;
        idx--;
    }
    return idx;
}

bool SelectableTextView::isWordChar(unsigned int cp) {
    if (cp == '_') return true;
    if (cp >= '0' && cp <= '9') return true;
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp > 127 && (ImCharIsBlankW((ImWchar)cp) == false)) return true;
    return false;
}

void SelectableTextView::selectWordAt(int bufferIndex) {
    if (buf.empty()) return;
    int n = (int)buf.size();
    int idx = clampIndexToCodepointBoundary(bufferIndex);
    if (idx >= n) idx = n - 1;
    if (idx < 0) return;

    int cp_start = clampIndexToCodepointBoundary(idx);
    const char* begin = buf.begin();
    unsigned int cp = 0;
    int len = ImTextCharFromUtf8(&cp, begin + cp_start, begin + n);
    if (len <= 0) {
        selectionStart = cp_start;
        selectionEnd = std::min(cp_start + 1, n);
        hasStoredSelection = selectionEnd > selectionStart;
        return;
    }

    bool wordChar = isWordChar(cp);

    int left = cp_start;
    while (left > 0) {
        int prev = left;
        do { prev--; } while (prev > 0 && ((unsigned char)buf[prev] & 0xC0) == 0x80);
        unsigned int cp2 = 0;
        int l2 = ImTextCharFromUtf8(&cp2, begin + prev, begin + n);
        if (l2 <= 0) break;
        if (isWordChar(cp2) != wordChar) break;
        left = prev;
    }

    int right = cp_start + len;
    while (right < n) {
        unsigned int cp2 = 0;
        int l2 = ImTextCharFromUtf8(&cp2, begin + right, begin + n);
        if (l2 <= 0) break;
        if (isWordChar(cp2) != wordChar) break;
        right += l2;
    }

    selectionStart = left;
    selectionEnd = right;
    hasStoredSelection = selectionEnd > selectionStart;
}

void SelectableTextView::selectLineAt(int bufferIndex) {
    if (buf.empty()) return;
    int n = (int)buf.size();
    int idx = std::max(0, std::min(bufferIndex, n));

    int start = idx;
    while (start > 0 && buf[start - 1] != '\n') start--;
    int end = idx;
    while (end < n && buf[end] != '\n') end++;

    selectionStart = start;
    selectionEnd = end;
    hasStoredSelection = selectionEnd > selectionStart;
}

void SelectableTextView::selectAll() {
    selectionStart = 0;
    selectionEnd = (int)buf.size();
    hasStoredSelection = selectionEnd > selectionStart;
}

std::string SelectableTextView::buildCopyText(int startInclusive, int endExclusive) const {
    int a = std::max(0, startInclusive);
    int b = std::min((int)buf.size(), endExclusive);
    if (b <= a) {
        return {};
    }

    const int totalLines = (lineOffsets.Size > 0) ? (lineOffsets.Size - 1) : 0;
    std::string out;
    out.reserve(static_cast<size_t>(b - a));

    for (int i = 0; i < totalLines; ++i) {
        int ls = lineOffsets[i];
        int le = (i + 1 < lineOffsets.Size) ? lineOffsets[i + 1] : (int)buf.size();
        if (le > 0 && le <= (int)buf.size() && buf[le - 1] == '\n') {
            le -= 1;
        }

        int s0 = std::max(a, ls);
        int e0 = std::min(b, le);
        if (e0 > s0) {
            out.append(buf.begin() + s0, buf.begin() + e0);
        }

        // Only emit a newline for real (hard) breaks whose '\n' lies in range;
        // soft-wrapped lines are rejoined.
        if (i < (int)lineHardBreak.size() && lineHardBreak[i]) {
            int nlPos = (i + 1 < lineOffsets.Size) ? lineOffsets[i + 1] - 1 : -1;
            if (nlPos >= 0 && nlPos < (int)buf.size() && buf[nlPos] == '\n' &&
                nlPos >= a && nlPos < b) {
                out.push_back('\n');
            }
        }
    }
    return out;
}

bool SelectableTextView::copySelectionToClipboard() const {
    if (!hasStoredSelection) return false;
    int a = std::min(selectionStart, selectionEnd);
    int b = std::max(selectionStart, selectionEnd);
    std::string out = buildCopyText(a, b);
    if (out.empty()) return false;
    ImGui::SetClipboardText(out.c_str());
    return true;
}

void SelectableTextView::copyAllToClipboard() const {
    if (buf.empty()) return;
    std::string out = buildCopyText(0, (int)buf.size());
    if (!out.empty()) {
        ImGui::SetClipboardText(out.c_str());
    }
}

void SelectableTextView::draw(const char* id, const ImVec2& size,
                              const std::vector<Paragraph>& paragraphs, bool scrollToBottom,
                              float lineSpacingY) {
    if (!ImGui::BeginChild(id, size, 0,
                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    float contentW = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
    float wrapW = ImMax(1.0f, contentW);

    size_t hash = paragraphs.size();
    for (const Paragraph& p : paragraphs) {
        hash ^= std::hash<std::string>{}(p.text) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        hash ^= static_cast<size_t>(p.color) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        hash ^= reinterpret_cast<size_t>(p.font) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    }
    if (hash != builtHash || fabsf(builtWrapWidth - wrapW) > 0.5f) {
        rebuild(paragraphs, wrapW);
        builtHash = hash;
        builtWrapWidth = wrapW;
    }

    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float lineBoxHeight = ImGui::GetTextLineHeight();
    const float rowSpacing = (lineSpacingY >= 0.0f)
        ? lineSpacingY
        : ImGui::GetStyle().ItemSpacing.y;
    const float lineAdvance = lineBoxHeight + rowSpacing;
    const int totalLines = (lineOffsets.Size > 0) ? (lineOffsets.Size - 1) : 0;

    if (lineSpacingY >= 0.0f) {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, lineSpacingY));
    }

    const bool winFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    const ImGuiIO& io = ImGui::GetIO();
    if (winFocused) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) selectAll();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelectionToClipboard();
    }

    auto lineStartIndex = [&](int line) -> int {
        return (line >= 0 && line < lineOffsets.Size) ? lineOffsets[line] : 0;
    };
    auto lineEndIndexExcl = [&](int line) -> int {
        if (line < 0 || line + 1 >= lineOffsets.Size) return (int)buf.size();
        int end = lineOffsets[line + 1];
        if (end > 0 && end <= (int)buf.size() && buf[end - 1] == '\n') end -= 1;
        return end;
    };
    auto lineFontFor = [&](int line) -> ImFont* {
        ImFont* lf = (line >= 0 && line < (int)lineFonts.size()) ? lineFonts[line] : nullptr;
        return lf ? lf : font;
    };

    int hoveredLine = -1;
    float hoveredLocalX = 0.0f;

    ImGuiListClipper clipper;
    clipper.Begin(totalLines, lineAdvance);
    const ImU32 selBg = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);

    int selA = selectionStart, selB = selectionEnd;
    if (selA > selB) std::swap(selA, selB);

    while (clipper.Step()) {
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
            int ls = lineStartIndex(line);
            int le = lineEndIndexExcl(line);
            ImVec2 linePos = ImGui::GetCursorScreenPos();

            ImRect hitRect(ImVec2(linePos.x, linePos.y),
                           ImVec2(linePos.x + wrapW, linePos.y + lineAdvance));
            if (hoveredLine == -1 && ImGui::IsMouseHoveringRect(hitRect.Min, hitRect.Max)) {
                hoveredLine = line;
                hoveredLocalX = ImGui::GetMousePos().x - linePos.x;
            }

            ImFont* lineFont = lineFontFor(line);
            const bool customFont = (lineFont != font);

            if (hasStoredSelection && selA >= 0 && selB > selA) {
                int hs = std::max(ls, selA);
                int he = std::min(le, selB);
                if (hs < he) {
                    const char* s0 = buf.begin() + ls;
                    const char* s1 = buf.begin() + hs;
                    const char* s2 = buf.begin() + he;
                    float x0 = lineFont->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, s0, s1).x;
                    float x1 = lineFont->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, s1, s2).x;
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2(linePos.x + x0, linePos.y),
                        ImVec2(linePos.x + x0 + x1, linePos.y + lineBoxHeight),
                        selBg);
                }
            }

            ImU32 col = (line >= 0 && line < (int)lineColors.size())
                ? lineColors[line] : ImGui::GetColorU32(ImGuiCol_Text);
            if (customFont) ImGui::PushFont(lineFont, fontSize);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ls < le) {
                ImGui::TextUnformatted(buf.begin() + ls, buf.begin() + le);
            } else {
                ImGui::TextUnformatted("");
            }
            ImGui::PopStyleColor();
            if (customFont) ImGui::PopFont();
        }
    }
    clipper.End();

    auto indexFromLineAndLocalX = [&](int line, float localX) -> int {
        if (line < 0 || totalLines <= 0) return 0;
        int ls = lineStartIndex(line);
        int le = lineEndIndexExcl(line);
        if (ls >= le) return ls;
        ImFont* lineFont = lineFontFor(line);
        const char* s = buf.begin() + ls;
        const char* e = buf.begin() + le;
        float x = 0.0f;
        int idx = ls;
        for (const char* p = s; p < e;) {
            unsigned int c;
            int len = ImTextCharFromUtf8(&c, p, e);
            if (len <= 0) break;
            float cw = lineFont->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, p, p + len).x;
            if (localX < x + cw * 0.5f) return idx;
            x += cw;
            p += len;
            idx += len;
        }
        return le;
    };

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
        if (hoveredLine >= 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredLine >= 0) {
            int idx = indexFromLineAndLocalX(hoveredLine, hoveredLocalX);
            int clicks = ImGui::GetIO().MouseClickedCount[ImGuiMouseButton_Left];
            if (clicks >= 3) {
                selectLineAt(idx);
                isSelecting = false;
            } else if (clicks == 2) {
                selectWordAt(idx);
                isSelecting = false;
            } else {
                selectionStart = selectionEnd = idx;
                hasStoredSelection = true;
                isSelecting = true;
            }
        } else if (isSelecting && ImGui::IsMouseDown(ImGuiMouseButton_Left) && hoveredLine >= 0) {
            int idx = indexFromLineAndLocalX(hoveredLine, hoveredLocalX);
            selectionEnd = idx;
            hasStoredSelection = (selectionStart != selectionEnd);
        }
    }
    if (isSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        isSelecting = false;
    }

    if (lineSpacingY >= 0.0f) {
        ImGui::PopStyleVar();
    }

    if (ImGui::BeginPopupContextWindow("##sel_ctx", ImGuiPopupFlags_MouseButtonRight)) {
        const bool hasContent = !buf.empty();
        int a = std::min(selectionStart, selectionEnd);
        int b = std::max(selectionStart, selectionEnd);
        const bool canCopy = hasStoredSelection && b > a;
        if (ImGui::MenuItem(ICON_FA_COPY " Copy", "Ctrl+C", false, canCopy)) {
            copySelectionToClipboard();
        }
        if (ImGui::MenuItem(ICON_FA_CLIPBOARD_LIST " Copy All", nullptr, false, hasContent)) {
            copyAllToClipboard();
        }
        if (ImGui::MenuItem(ICON_FA_I_CURSOR " Select All", "Ctrl+A", false, hasContent)) {
            selectAll();
        }
        ImGui::EndPopup();
    }

    // Sticky auto-scroll: stay pinned to the bottom across content growth, and
    // release only when the user scrolls up. Re-pinning every frame converges
    // even when a large message is appended in a single frame.
    float currentScrollY = ImGui::GetScrollY();
    float maxScrollY = ImGui::GetScrollMaxY();
    if (!isSelecting && fabsf(currentScrollY - lastScrollY) > 0.5f) {
        stickToBottom = (currentScrollY >= maxScrollY - 1.0f);
    }
    if (scrollToBottom) {
        stickToBottom = true;
    }
    if (stickToBottom && !isSelecting) {
        ImGui::SetScrollY(maxScrollY);
        currentScrollY = maxScrollY;
    }
    lastScrollY = currentScrollY;

    ImGui::EndChild();
}

} // namespace doriax::editor
