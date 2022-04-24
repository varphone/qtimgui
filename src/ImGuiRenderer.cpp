#include "ImGuiRenderer.h"

#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QGuiApplication>
#include <QMouseEvent>

#ifdef ANDROID
#define GL_VERTEX_ARRAY_BINDING 0x85B5 // Missing in android as of May 2020
#define USE_GLSL_ES
#endif

#ifdef USE_GLSL_ES
#define IMGUIRENDERER_GLSL_VERSION "#version 300 es\n"
#else
#define IMGUIRENDERER_GLSL_VERSION "#version 330\n"
#endif

namespace QtImGui {

namespace {

// Keyboard mapping.
//
// Dear ImGui use those indices to peek into the io.KeysDown[] array.
const QHash<int, ImGuiKey> keyMap = {
    {Qt::Key_Tab, ImGuiKey_Tab},
    {Qt::Key_Left, ImGuiKey_LeftArrow},
    {Qt::Key_Right, ImGuiKey_RightArrow},
    {Qt::Key_Up, ImGuiKey_UpArrow},
    {Qt::Key_Down, ImGuiKey_DownArrow},
    {Qt::Key_PageUp, ImGuiKey_PageUp},
    {Qt::Key_PageDown, ImGuiKey_PageDown},
    {Qt::Key_Home, ImGuiKey_Home},
    {Qt::Key_End, ImGuiKey_End},
    {Qt::Key_Insert, ImGuiKey_Insert},
    {Qt::Key_Delete, ImGuiKey_Delete},
    {Qt::Key_Backspace, ImGuiKey_Backspace},
    {Qt::Key_Space, ImGuiKey_Space},
    {Qt::Key_Enter, ImGuiKey_Enter},
    {Qt::Key_Return, ImGuiKey_Enter},
    {Qt::Key_Escape, ImGuiKey_Escape},
    {Qt::Key_A, ImGuiKey_A},
    {Qt::Key_C, ImGuiKey_C},
    {Qt::Key_V, ImGuiKey_V},
    {Qt::Key_X, ImGuiKey_X},
    {Qt::Key_Y, ImGuiKey_Y},
    {Qt::Key_Z, ImGuiKey_Z},
};

#ifndef QT_NO_CURSOR
const QHash<ImGuiMouseCursor, Qt::CursorShape> cursorMap = {
    {ImGuiMouseCursor_Arrow, Qt::CursorShape::ArrowCursor},
    {ImGuiMouseCursor_TextInput, Qt::CursorShape::IBeamCursor},
    {ImGuiMouseCursor_ResizeAll, Qt::CursorShape::SizeAllCursor},
    {ImGuiMouseCursor_ResizeNS, Qt::CursorShape::SizeVerCursor},
    {ImGuiMouseCursor_ResizeEW, Qt::CursorShape::SizeHorCursor},
    {ImGuiMouseCursor_ResizeNESW, Qt::CursorShape::SizeBDiagCursor},
    {ImGuiMouseCursor_ResizeNWSE, Qt::CursorShape::SizeFDiagCursor},
    {ImGuiMouseCursor_Hand, Qt::CursorShape::PointingHandCursor},
    {ImGuiMouseCursor_NotAllowed, Qt::CursorShape::ForbiddenCursor},
};
#endif

static QByteArray gLastClipboardText;

} // namespace

void ImGuiRenderer::initialize(WindowWrapper* window)
{
    mWindow.reset(window);
    initializeOpenGLFunctions();

    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major == 0 && minor == 0) {
        // Query GL_VERSION in desktop GL 2.x,
        // the string will start with "<major>.<minor>"
        const char* glVersion = (const char*)glGetString(GL_VERSION);
        sscanf(glVersion, "%d.%d", &major, &minor);
    }

    mGlVersion = (GLuint)(major * 100 + minor * 10);

    mCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(mCtx);

    createDeviceObjects();

    // Setup backend capabilities flags
    ImGuiIO& io = ImGui::GetIO();

#ifndef QT_NO_CURSOR
    io.BackendFlags |=
        ImGuiBackendFlags_HasMouseCursors; // We can honor GetMouseCursor()
                                           // values (optional)
    io.BackendFlags |=
        ImGuiBackendFlags_HasSetMousePos; // We can honor io.WantSetMousePos
                                          // requests (optional, rarely used)
#endif

    io.BackendPlatformName = "qtimgui";

    // Setup keyboard mapping
    for (ImGuiKey key : keyMap.values()) {
        io.AddKeyEvent(key, false);
    }

    // io.RenderDrawListsFn = [](ImDrawData *drawData) {
    //    instance()->renderDrawList(drawData);
    // };

    io.SetClipboardTextFn = [](void* userData, const char* text) {
        Q_UNUSED(userData);
        QGuiApplication::clipboard()->setText(text);
    };
    io.GetClipboardTextFn = [](void* userData) {
        Q_UNUSED(userData);
        gLastClipboardText = QGuiApplication::clipboard()->text().toUtf8();
        return gLastClipboardText.constData();
    };

    window->installEventFilter(this);
}

void ImGuiRenderer::setupRenderStates(ImDrawData* drawData, int fbWidth,
                                      int fbHeight, GLuint vertexArrayObject)
{
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                        GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);

    // Setup viewport, orthographic projection matrix
    // Our visible imgui space lies from drawData->DisplayPos (top left) to
    // drawData->DisplayPos + drawData->DisplaySize (bottom right).
    // DisplayPos is (0,0) for single viewport apps.
    glViewport(0, 0, (GLsizei)fbWidth, (GLsizei)fbHeight);
    float L = drawData->DisplayPos.x;
    float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
    float T = drawData->DisplayPos.y;
    float B = drawData->DisplayPos.y + drawData->DisplaySize.y;

    const float orthoProjection[4][4] = {
        {2.0f / (R - L), 0.0f, 0.0f, 0.0f},
        {0.0f, 2.0f / (T - B), 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f, 0.0f},
        {(R + L) / (L - R), (T + B) / (B - T), 0.0f, 1.0f},
    };

    glUseProgram(mShaderHandle);
    glUniform1i(mAttribLocationTex, 0);
    glUniformMatrix4fv(mAttribLocationProjMtx, 1, GL_FALSE,
                       &orthoProjection[0][0]);

    glBindVertexArray(vertexArrayObject);

    // Bind vertex/index buffers and setup attributes for ImDrawVert
    glBindBuffer(GL_ARRAY_BUFFER, mVboHandle);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mElementsHandle);
    glEnableVertexAttribArray(mAttribLocationVtxPos);
    glEnableVertexAttribArray(mAttribLocationVtxUV);
    glEnableVertexAttribArray(mAttribLocationVtxColor);
    glVertexAttribPointer(mAttribLocationVtxPos, 2, GL_FLOAT, GL_FALSE,
                          sizeof(ImDrawVert),
                          (GLvoid*)IM_OFFSETOF(ImDrawVert, pos));
    glVertexAttribPointer(mAttribLocationVtxUV, 2, GL_FLOAT, GL_FALSE,
                          sizeof(ImDrawVert),
                          (GLvoid*)IM_OFFSETOF(ImDrawVert, uv));
    glVertexAttribPointer(mAttribLocationVtxColor, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                          sizeof(ImDrawVert),
                          (GLvoid*)IM_OFFSETOF(ImDrawVert, col));
}

void ImGuiRenderer::renderDrawList(ImDrawData* drawData)
{
    // Avoid rendering when minimized, scale coordinates for retina displays
    // (screen coordinates != framebuffer coordinates)
    const ImGuiIO& io = ImGui::GetIO();

    int fbWidth = (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x);
    int fbHeight = (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y);
    if (fbWidth == 0 || fbHeight == 0)
        return;

    drawData->ScaleClipRects(io.DisplayFramebufferScale);

    // Backup GL state
    GLint lastActiveTexture;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &lastActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    GLint lastProgram;
    glGetIntegerv(GL_CURRENT_PROGRAM, &lastProgram);
    GLint lastTexture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
    GLint lastArrayBuffer;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastArrayBuffer);
    GLint lastElementArrayBuffer;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &lastElementArrayBuffer);
    GLint lastVertexArray;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &lastVertexArray);
    GLint lastBlendSrcRgb;
    glGetIntegerv(GL_BLEND_SRC_RGB, &lastBlendSrcRgb);
    GLint lastBlendDstRgb;
    glGetIntegerv(GL_BLEND_DST_RGB, &lastBlendDstRgb);
    GLint lastBlendSrcAlpha;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &lastBlendSrcAlpha);
    GLint lastBlendDstAlpha;
    glGetIntegerv(GL_BLEND_DST_ALPHA, &lastBlendDstAlpha);
    GLint lastBlendEquationRgb;
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &lastBlendEquationRgb);
    GLint lastBlendEquationAlpha;
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &lastBlendEquationAlpha);
    GLint lastViewport[4];
    glGetIntegerv(GL_VIEWPORT, lastViewport);
    GLint lastScissorBox[4];
    glGetIntegerv(GL_SCISSOR_BOX, lastScissorBox);
    GLboolean lastEnableBlend = glIsEnabled(GL_BLEND);
    GLboolean lastEnableCullFace = glIsEnabled(GL_CULL_FACE);
    GLboolean lastEnableDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean lastEnableScissorTest = glIsEnabled(GL_SCISSOR_TEST);

    // Setup desired GL state
    // Recreate the VAO every time (this is to easily allow multiple GL contexts
    // to be rendered to. VAO are not shared among GL contexts) The renderer
    // would actually work without any VAO bound, but then our VertexAttrib
    // calls would overwrite the default one currently bound.
    GLuint vertexArrayObject = 0;
    glGenVertexArrays(1, &vertexArrayObject);

    setupRenderStates(drawData, fbWidth, fbHeight, vertexArrayObject);

    // Will project scissor/clipping rectangles into framebuffer space
    // (0,0) unless using multi-viewports
    ImVec2 clipOff = drawData->DisplayPos;
    // (1,1) unless using retina display which are often (2,2)
    ImVec2 clipScale = drawData->FramebufferScale;

    for (int n = 0; n < drawData->CmdListsCount; n++) {
        const ImDrawList* cmdList = drawData->CmdLists[n];

        // Upload vertex/index buffers
        GLsizeiptr vtxBufferSize =
            (GLsizeiptr)cmdList->VtxBuffer.Size * (int)sizeof(ImDrawVert);
        GLsizeiptr idxBufferSize =
            (GLsizeiptr)cmdList->IdxBuffer.Size * (int)sizeof(ImDrawIdx);
        if (mVertexBufferSize < vtxBufferSize) {
            mVertexBufferSize = vtxBufferSize;
            glBufferData(GL_ARRAY_BUFFER, mVertexBufferSize, NULL,
                         GL_STREAM_DRAW);
        }
        if (mIndexBufferSize < idxBufferSize) {
            mIndexBufferSize = idxBufferSize;
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, mIndexBufferSize, NULL,
                         GL_STREAM_DRAW);
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, vtxBufferSize,
                        (const GLvoid*)cmdList->VtxBuffer.Data);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, idxBufferSize,
                        (const GLvoid*)cmdList->IdxBuffer.Data);

        for (int i = 0; i < cmdList->CmdBuffer.Size; i++) {
            const ImDrawCmd* pcmd = &cmdList->CmdBuffer[i];
            if (pcmd->UserCallback) {
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    setupRenderStates(drawData, fbWidth, fbHeight,
                                      vertexArrayObject);
                }
                else {
                    pcmd->UserCallback(cmdList, pcmd);
                }
            }
            else if (pcmd->ElemCount) {
                void* idxBufferOffset =
                    (void*)(intptr_t)(pcmd->IdxOffset * sizeof(ImDrawIdx));
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 clipMin((pcmd->ClipRect.x - clipOff.x) * clipScale.x,
                               (pcmd->ClipRect.y - clipOff.y) * clipScale.y);
                ImVec2 clipMax((pcmd->ClipRect.z - clipOff.x) * clipScale.x,
                               (pcmd->ClipRect.w - clipOff.y) * clipScale.y);
                if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
                    continue;

                // Apply scissor/clipping rectangle (Y is inverted in OpenGL)
                glScissor((int)clipMin.x, (int)((float)fbHeight - clipMax.y),
                          (int)(clipMax.x - clipMin.x),
                          (int)(clipMax.y - clipMin.y));

                // Bind texture, Draw
                glBindTexture(GL_TEXTURE_2D, (GLuint)(size_t)pcmd->GetTexID());
                glDrawElements(GL_TRIANGLES, (GLsizei)pcmd->ElemCount,
                               sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT
                                                      : GL_UNSIGNED_INT,
                               idxBufferOffset);
            }
        }
    }

    // Destroy the temporary VAO
    glDeleteVertexArrays(1, &vertexArrayObject);

    // Restore modified GL state
    glUseProgram(lastProgram);
    glBindTexture(GL_TEXTURE_2D, lastTexture);
    glActiveTexture(lastActiveTexture);
    glBindVertexArray(lastVertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, lastArrayBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lastElementArrayBuffer);
    glBlendEquationSeparate(lastBlendEquationRgb, lastBlendEquationAlpha);
    glBlendFuncSeparate(lastBlendSrcRgb, lastBlendDstRgb, lastBlendSrcAlpha,
                        lastBlendDstAlpha);
    if (lastEnableBlend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (lastEnableCullFace)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    if (lastEnableDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (lastEnableScissorTest)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    glViewport(lastViewport[0], lastViewport[1], (GLsizei)lastViewport[2],
               (GLsizei)lastViewport[3]);
    glScissor(lastScissorBox[0], lastScissorBox[1], (GLsizei)lastScissorBox[2],
              (GLsizei)lastScissorBox[3]);
}

bool ImGuiRenderer::createFontsTexture()
{
    // Build texture atlas
    ImGuiIO& io = ImGui::GetIO();
    int width = 0;
    int height = 0;
    unsigned char* pixels = nullptr;
    // Load as RGBA 32-bits (75% of the memory is wasted,
    // but default font is so small) because it is more likely to be
    // compatible with user's existing shaders.
    // If your ImTextureId represent a higher-level concept than just
    // a GL texture id, consider calling GetTexDataAsAlpha8() instead
    // to save on GPU memory.
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    // Upload texture to graphics system
    GLint lastTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
    glGenTextures(1, &mFontTexture);
    glBindTexture(GL_TEXTURE_2D, mFontTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);

    // Store our identifier
    io.Fonts->SetTexID(
        reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(mFontTexture)));

    // Restore state
    glBindTexture(GL_TEXTURE_2D, lastTexture);

    return true;
}

bool ImGuiRenderer::createDeviceObjects()
{
    // Backup GL state
    GLint lastTexture, lastArrayBuffer, lastVertexArray;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastArrayBuffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &lastVertexArray);

    const GLchar* vertex_shader = IMGUIRENDERER_GLSL_VERSION
        "uniform mat4 ProjMtx;\n"
        "in vec2 Position;\n"
        "in vec2 UV;\n"
        "in vec4 Color;\n"
        "out vec2 Frag_UV;\n"
        "out vec4 Frag_Color;\n"
        "void main()\n"
        "{\n"
        "	Frag_UV = UV;\n"
        "	Frag_Color = Color;\n"
        "	gl_Position = ProjMtx * vec4(Position.xy,0,1);\n"
        "}\n";

    const GLchar* fragment_shader = IMGUIRENDERER_GLSL_VERSION
        "precision mediump float;"
        "uniform sampler2D Texture;\n"
        "in vec2 Frag_UV;\n"
        "in vec4 Frag_Color;\n"
        "out vec4 Out_Color;\n"
        "void main()\n"
        "{\n"
        "	Out_Color = Frag_Color * texture( Texture, Frag_UV.st);\n"
        "}\n";

    mShaderHandle = glCreateProgram();
    mVertHandle = glCreateShader(GL_VERTEX_SHADER);
    mFragHandle = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(mVertHandle, 1, &vertex_shader, 0);
    glShaderSource(mFragHandle, 1, &fragment_shader, 0);
    glCompileShader(mVertHandle);
    glCompileShader(mFragHandle);
    glAttachShader(mShaderHandle, mVertHandle);
    glAttachShader(mShaderHandle, mFragHandle);
    glLinkProgram(mShaderHandle);

    mAttribLocationTex = glGetUniformLocation(mShaderHandle, "Texture");
    mAttribLocationProjMtx = glGetUniformLocation(mShaderHandle, "ProjMtx");
    mAttribLocationVtxPos = glGetAttribLocation(mShaderHandle, "Position");
    mAttribLocationVtxUV = glGetAttribLocation(mShaderHandle, "UV");
    mAttribLocationVtxColor = glGetAttribLocation(mShaderHandle, "Color");

    glGenBuffers(1, &mVboHandle);
    glGenBuffers(1, &mElementsHandle);

    glBindBuffer(GL_ARRAY_BUFFER, mVboHandle);
    glEnableVertexAttribArray(mAttribLocationVtxPos);
    glEnableVertexAttribArray(mAttribLocationVtxUV);
    glEnableVertexAttribArray(mAttribLocationVtxColor);

#define OFFSETOF(TYPE, ELEMENT) ((size_t) & (((TYPE*)0)->ELEMENT))
    glVertexAttribPointer(mAttribLocationVtxPos, 2, GL_FLOAT, GL_FALSE,
                          sizeof(ImDrawVert),
                          (GLvoid*)OFFSETOF(ImDrawVert, pos));
    glVertexAttribPointer(mAttribLocationVtxUV, 2, GL_FLOAT, GL_FALSE,
                          sizeof(ImDrawVert),
                          (GLvoid*)OFFSETOF(ImDrawVert, uv));
    glVertexAttribPointer(mAttribLocationVtxColor, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                          sizeof(ImDrawVert),
                          (GLvoid*)OFFSETOF(ImDrawVert, col));
#undef OFFSETOF

    createFontsTexture();

    // Restore modified GL state
    glBindTexture(GL_TEXTURE_2D, lastTexture);
    glBindBuffer(GL_ARRAY_BUFFER, lastArrayBuffer);
    glBindVertexArray(lastVertexArray);

    return true;
}

void ImGuiRenderer::newFrame()
{
    ImGuiIO& io = ImGui::GetIO();

    // Setup display size (every frame to accommodate for window resizing)
    io.DisplaySize = ImVec2(mWindow->size().width(), mWindow->size().height());
    io.DisplayFramebufferScale =
        ImVec2(mWindow->devicePixelRatio(), mWindow->devicePixelRatio());

    // Setup time step
    double currentTime = QDateTime::currentMSecsSinceEpoch() / double(1000);
    io.DeltaTime = mTime > 0.0 ? (float)(currentTime - mTime)
                               : (float)(1.0f / 60.0f);
    mTime = currentTime;

    // If ImGui wants to set cursor position (for example, during navigation by
    // using keyboard) we need to do it here (before getting `QCursor::pos()`
    // below).
    setCursorPos(io);

    // Setup inputs
    // (we already got mouse wheel, keyboard keys & characters from glfw
    // callbacks polled in glfwPollEvents())
    if (mWindow->isActive()) {
        // Mouse position in screen coordinates (set to -1,-1 if no mouse / on
        // another screen, etc.)
        const QPoint pos = mWindow->mapFromGlobal(QCursor::pos());
        io.AddMousePosEvent(pos.x(), pos.y());
    }
    else {
        io.AddMousePosEvent(-1, -1);
    }

    updateCursorShape(io);

    // Start the frame
    ImGui::NewFrame();
}

void ImGuiRenderer::render()
{
    auto drawData = ImGui::GetDrawData();
    renderDrawList(drawData);
}

ImGuiRenderer::ImGuiRenderer() : mWindow()
{
}

ImGuiRenderer::~ImGuiRenderer()
{
    // Remove this context
    ImGui::DestroyContext(mCtx);
}

void ImGuiRenderer::onMousePressedChange(QMouseEvent* event)
{
    // Select current context
    ImGui::SetCurrentContext(mCtx);

    ImGuiIO& io = ImGui::GetIO();

    io.AddMouseButtonEvent(ImGuiMouseButton_Left,
                           event->buttons() & Qt::LeftButton);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right,
                           event->buttons() & Qt::RightButton);
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle,
                           event->buttons() & Qt::MiddleButton);
}

void ImGuiRenderer::onWheel(QWheelEvent* event)
{
    // Select current context
    ImGui::SetCurrentContext(mCtx);

    ImGuiIO& io = ImGui::GetIO();

    float wx = 0;
    float wy = 0;

    // Handle horizontal component
    if (event->pixelDelta().x() != 0) {
        wx = event->pixelDelta().x() / (ImGui::GetTextLineHeight());
    }
    else {
        // Magic number of 120 comes from Qt doc on QWheelEvent::pixelDelta()
        wx = event->angleDelta().x() / 120.0f;
    }

    // Handle vertical component
    if (event->pixelDelta().y() != 0) {
        // 5 lines per unit
        wy = event->pixelDelta().y() / (5.0 * ImGui::GetTextLineHeight());
    }
    else {
        // Magic number of 120 comes from Qt doc on QWheelEvent::pixelDelta()
        wy = event->angleDelta().y() / 120.0f;
    }

    io.AddMouseWheelEvent(wx, wy);
}

void ImGuiRenderer::onKeyPressRelease(QKeyEvent* event)
{
    // Select current context
    ImGui::SetCurrentContext(mCtx);

    ImGuiIO& io = ImGui::GetIO();

    const bool key_pressed = (event->type() == QEvent::KeyPress);

    // Translate `Qt::Key` into `ImGuiKey`, and apply 'pressed' state for that
    // key
    const auto key_it = keyMap.constFind(event->key());
    if (key_it != keyMap.constEnd()) { // Qt's key found in keyMap
        const int imgui_key = *(key_it);
        io.AddKeyEvent(imgui_key, key_pressed);
    }

    if (key_pressed) {
        const QString text = event->text();
        if (text.size() == 1) {
            io.AddInputCharacter(text.at(0).unicode());
        }
    }

#ifdef Q_OS_MAC
    io.AddKeyEvent(ImGuiKey_ModCtrl, event->modifiers() & Qt::MetaModifier);
    io.AddKeyEvent(ImGuiKey_ModShift, event->modifiers() & Qt::ShiftModifier);
    io.AddKeyEvent(ImGuiKey_ModAlt, event->modifiers() & Qt::AltModifier);
    io.AddKeyEvent(ImGuiKey_ModSuper, event->modifiers() & Qt::ControlModifier);
#else
    io.AddKeyEvent(ImGuiKey_ModCtrl, event->modifiers() & Qt::ControlModifier);
    io.AddKeyEvent(ImGuiKey_ModShift, event->modifiers() & Qt::ShiftModifier);
    io.AddKeyEvent(ImGuiKey_ModAlt, event->modifiers() & Qt::AltModifier);
    io.AddKeyEvent(ImGuiKey_ModSuper, event->modifiers() & Qt::MetaModifier);
#endif
}

void ImGuiRenderer::updateCursorShape(const ImGuiIO& io)
{
    // NOTE: This code will be executed, only if the following flags have been
    // set:
    // - backend flag: `ImGuiBackendFlags_HasMouseCursors`    - enabled
    // - config  flag: `ImGuiConfigFlags_NoMouseCursorChange` - disabled

#ifndef QT_NO_CURSOR
    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
        return;

    const ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
    if (io.MouseDrawCursor || (imgui_cursor == ImGuiMouseCursor_None)) {
        // Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
        mWindow->setCursorShape(Qt::CursorShape::BlankCursor);
    }
    else {
        // Show OS mouse cursor

        // Translate `ImGuiMouseCursor` into `Qt::CursorShape` and show it, if
        // we can
        const auto cursor_it = cursorMap.constFind(imgui_cursor);
        if (cursor_it !=
            cursorMap
                .constEnd()) // `Qt::CursorShape` found for `ImGuiMouseCursor`
        {
            const Qt::CursorShape qt_cursor_shape = *(cursor_it);
            mWindow->setCursorShape(qt_cursor_shape);
        }
        else // shape NOT found - use default
        {
            mWindow->setCursorShape(Qt::CursorShape::ArrowCursor);
        }
    }
#else
    Q_UNUSED(io);
#endif
}

void ImGuiRenderer::setCursorPos(const ImGuiIO& io)
{
    // NOTE: This code will be executed, only if the following flags have been
    // set:
    // - backend flag: `ImGuiBackendFlags_HasSetMousePos`      - enabled
    // - config  flag: `ImGuiConfigFlags_NavEnableSetMousePos` - enabled

#ifndef QT_NO_CURSOR
    if (io.WantSetMousePos) {
        mWindow->setCursorPos({(int)io.MousePos.x, (int)io.MousePos.y});
    }
#else
    Q_UNUSED(io);
#endif
}

bool ImGuiRenderer::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mWindow->object()) {
        switch (event->type()) {
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
            this->onMousePressedChange(static_cast<QMouseEvent*>(event));
            break;
        case QEvent::Wheel:
            this->onWheel(static_cast<QWheelEvent*>(event));
            break;
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            this->onKeyPressRelease(static_cast<QKeyEvent*>(event));
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

ImGuiRenderer* ImGuiRenderer::instance()
{
    static ImGuiRenderer* instance = nullptr;
    if (!instance) {
        instance = new ImGuiRenderer();
    }
    return instance;
}

} // namespace QtImGui
