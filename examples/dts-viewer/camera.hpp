#ifndef DTS_VIEWER_CAMERA_HPP
#define DTS_VIEWER_CAMERA_HPP

// Free-fly first-person camera — Spec 01 (08-walkable-viewer track).
//
// Convention: yaw=0 faces +Z; right-hand Y-up coordinate system.
// forward = (sin(yaw)*cos(pitch),  sin(pitch),  cos(yaw)*cos(pitch))
// right   = (cos(yaw),             0,          -sin(yaw))
//
// Mouse input: SDL relative-mouse-mode delivers raw deltas in SDL_MOUSEMOTION
// events; call handle_mouse_motion(c, dx, dy) from the event loop.
// Key input: call update_camera_free(c, dt) once per frame; it reads
// SDL_GetKeyboardState internally (WASD + Space/Ctrl + Shift sprint +
// +/- FOV). Escape toggles mouse capture.

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL.h>
#include <cmath>

namespace dts_viewer
{

struct Camera
{
    glm::vec3 position  {0.0f, 50.0f, 0.0f};
    float yaw           = 0.0f;      // radians; 0 = +Z forward
    float pitch         = 0.0f;      // radians; clamped ±1.55
    float move_speed    = 40.0f;     // metres per second
    float sprint_mult   = 4.0f;
    float mouse_sens    = 0.0025f;   // radians per raw pixel
    float fov_deg       = 75.0f;
    float near_plane    = 0.5f;
    float far_plane     = 5000.0f;
    bool  mouse_captured = false;
};

inline glm::vec3 camera_forward(const Camera& c)
{
    return glm::vec3(
        std::sin(c.yaw) * std::cos(c.pitch),
        std::sin(c.pitch),
        std::cos(c.yaw) * std::cos(c.pitch));
}

inline glm::vec3 camera_right(const Camera& c)
{
    return glm::vec3(std::cos(c.yaw), 0.0f, -std::sin(c.yaw));
}

inline glm::mat4 camera_view(const Camera& c)
{
    const glm::vec3 fwd = camera_forward(c);
    return glm::lookAt(c.position, c.position + fwd, glm::vec3(0, 1, 0));
}

inline glm::mat4 camera_projection(const Camera& c, float aspect)
{
    return glm::perspective(glm::radians(c.fov_deg), aspect, c.near_plane, c.far_plane);
}

// Call from the SDL_MOUSEMOTION event branch when mouse_captured is true.
inline void handle_mouse_motion(Camera& c, int dx, int dy)
{
    // yaw decreases on dx>0 because camera_forward uses sin(yaw) for +X.
    c.yaw   -= static_cast<float>(dx) * c.mouse_sens;
    c.pitch -= static_cast<float>(dy) * c.mouse_sens;
    c.pitch  = glm::clamp(c.pitch, -1.55f, 1.55f);
}

// Call once per frame to move the camera from keyboard state.
// Escape toggles mouse capture.
inline void update_camera_free(Camera& c, float dt)
{
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    bool sprint = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    float speed = c.move_speed * (sprint ? c.sprint_mult : 1.0f) * dt;

    const glm::vec3 fwd   = camera_forward(c);
    const glm::vec3 right = camera_right(c);

    if (keys[SDL_SCANCODE_W]) c.position += fwd   * speed;
    if (keys[SDL_SCANCODE_S]) c.position -= fwd   * speed;
    if (keys[SDL_SCANCODE_A]) c.position -= right * speed;
    if (keys[SDL_SCANCODE_D]) c.position += right * speed;
    if (keys[SDL_SCANCODE_SPACE])  c.position.y += speed;
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) c.position.y -= speed;

    // FOV adjust: + / - at 30 deg/s.
    if (keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS])
        c.fov_deg = glm::clamp(c.fov_deg + 30.0f * dt, 40.0f, 110.0f);
    if (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS])
        c.fov_deg = glm::clamp(c.fov_deg - 30.0f * dt, 40.0f, 110.0f);
}

// Toggle SDL relative mouse mode.  Returns new capture state.
inline bool toggle_mouse_capture(Camera& c)
{
    c.mouse_captured = !c.mouse_captured;
    SDL_SetRelativeMouseMode(c.mouse_captured ? SDL_TRUE : SDL_FALSE);
    return c.mouse_captured;
}

// Set capture state explicitly without toggle.
inline void set_mouse_capture(Camera& c, bool captured)
{
    c.mouse_captured = captured;
    SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE);
}

} // namespace dts_viewer

#endif // DTS_VIEWER_CAMERA_HPP
