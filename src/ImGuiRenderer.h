#pragma once

#include <QObject>
#include <QOpenGLExtraFunctions>
#include <QPoint>
#include <imgui.h>
#include <memory>

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;

namespace QtImGui {

class WindowWrapper {
public:
    virtual ~WindowWrapper()
    {
    }
    virtual void installEventFilter(QObject* object) = 0;
    virtual QSize size() const = 0;
    virtual qreal devicePixelRatio() const = 0;
    virtual bool isActive() const = 0;
    virtual QPoint mapFromGlobal(const QPoint& p) const = 0;
    virtual QObject* object() = 0;

    virtual void setCursorShape(Qt::CursorShape shape) = 0;
    virtual void setCursorPos(const QPoint& localPos) = 0;
};

class ImGuiRenderer : public QObject, private QOpenGLExtraFunctions {
    Q_OBJECT
public:
    void initialize(WindowWrapper* window);
    void newFrame();
    void render();
    bool eventFilter(QObject* watched, QEvent* event);

    static ImGuiRenderer* instance();

public:
    ImGuiRenderer();
    ~ImGuiRenderer();

private:
    void onMousePressedChange(QMouseEvent* event);
    void onWheel(QWheelEvent* event);
    void onKeyPressRelease(QKeyEvent* event);

    void updateCursorShape(const ImGuiIO& io);
    void setCursorPos(const ImGuiIO& io);

    void setupRenderStates(ImDrawData* drawData, int fbWidth, int fbHeight,
                           GLuint vertexArrayObject);
    void renderDrawList(ImDrawData* drawData);
    bool createFontsTexture();
    bool createDeviceObjects();

    std::unique_ptr<WindowWrapper> mWindow;
    ImGuiContext* mCtx = nullptr;
    double mTime = 0.0f;
    GLuint mGlVersion = 0;
    GLuint mFontTexture = 0;
    GLuint mShaderHandle = 0;
    GLuint mVertHandle = 0;
    GLuint mFragHandle = 0;
    GLint mAttribLocationTex = 0;
    GLint mAttribLocationProjMtx = 0;
    GLint mAttribLocationVtxPos = 0;
    GLint mAttribLocationVtxUV = 0;
    GLint mAttribLocationVtxColor = 0;
    GLuint mVboHandle = 0;
    GLuint mElementsHandle = 0;
    GLsizeiptr mVertexBufferSize = 0;
    GLsizeiptr mIndexBufferSize = 0;
};

} // namespace QtImGui
