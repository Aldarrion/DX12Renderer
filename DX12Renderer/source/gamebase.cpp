#include "application.h"
#include "gamebase.h"
#include "window.h"

#include <cassert>
#include <directxmath.h>

GameBase::GameBase(const std::wstring& name, int width, int height, bool vSync)
    : m_Name(name)
    , m_Width(width)
    , m_Height(height)
    , m_vSync(vSync) {
}

GameBase::~GameBase() {
    assert(!m_pWindow && "Use Game::Destroy() before destruction.");
}

bool GameBase::Initialize() {
    // Check for DirectX Math library support.
    if (!DirectX::XMVerifyCPUSupport()) {
        MessageBoxA(NULL, "Failed to verify DirectX Math library support.", "Error", MB_OK | MB_ICONERROR);
        return false;
    }

    m_pWindow = Application::Get().CreateRenderWindow(m_Name, m_Width, m_Height, m_vSync);
    m_pWindow->RegisterCallbacks(shared_from_this());
    m_pWindow->Show();

    return true;
}

void GameBase::Destroy() {
    Application::Get().DestroyWindow(m_pWindow);
    m_pWindow.reset();
}

void GameBase::OnUpdate(UpdateEventArgs& e) {

}

void GameBase::OnRender(RenderEventArgs& e) {

}

void GameBase::OnKeyPressed(KeyEventArgs& e) {
    // By default, do nothing.
}

void GameBase::OnKeyReleased(KeyEventArgs& e) {
    // By default, do nothing.
}

void GameBase::OnMouseMoved(class MouseMotionEventArgs& e) {
    // By default, do nothing.
}

void GameBase::OnMouseButtonPressed(MouseButtonEventArgs& e) {
    // By default, do nothing.
}

void GameBase::OnMouseButtonReleased(MouseButtonEventArgs& e) {
    // By default, do nothing.
}

void GameBase::OnMouseWheel(MouseWheelEventArgs& e) {
    // By default, do nothing.
}

void GameBase::OnResize(ResizeEventArgs& e) {
    m_Width = e.Width;
    m_Height = e.Height;
}

void GameBase::OnWindowDestroy() {
    // If the Window which we are registered to is 
    // destroyed, then any resources which are associated 
    // to the window must be released.
    UnloadContent();
}
