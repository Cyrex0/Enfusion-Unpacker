/**
 * Enfusion Unpacker - Camera
 */

#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace enfusion {

/**
 * Orbital camera for 3D model viewing.
 */
class Camera {
public:
    Camera();

    void set_target(const glm::vec3& target);
    void set_distance(float distance);
    void set_angles(float yaw, float pitch);
    void set_fov(float fov);
    void set_aspect(float aspect);
    void set_clip(float near, float far);

    void orbit(float delta_yaw, float delta_pitch);
    void pan(float delta_x, float delta_y);
    void zoom(float delta);
    void reset();

    glm::vec3 position() const;
    glm::mat4 view_matrix() const;
    glm::mat4 projection_matrix() const;
    glm::mat4 view_projection_matrix() const;

private:
    void update_matrices();

    // Camera parameters
    float fov_ = 45.0f;
    float aspect_ = 16.0f / 9.0f;
    float near_ = 0.1f;
    float far_ = 1000.0f;

    // Orbital parameters
    float yaw_ = 45.0f;
    float pitch_ = 30.0f;
    float distance_ = 5.0f;

    // Computed values
    glm::vec3 position_;
    glm::vec3 target_;
    glm::mat4 view_matrix_;
    glm::mat4 projection_matrix_;
};

} // namespace enfusion
