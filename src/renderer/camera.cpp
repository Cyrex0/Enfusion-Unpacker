/**
 * Enfusion Unpacker - Camera Implementation
 */

#include "renderer/camera.hpp"
#include <algorithm>
#include <cmath>

namespace enfusion {

Camera::Camera()
    : target_(0.0f, 0.0f, 0.0f)
    , distance_(5.0f)
    , yaw_(45.0f)
    , pitch_(30.0f)
    , fov_(45.0f)
    , aspect_(16.0f / 9.0f)
    , near_(0.1f)
    , far_(1000.0f) {
    update_matrices();
}

void Camera::set_target(const glm::vec3& target) {
    target_ = target;
    update_matrices();
}

void Camera::set_distance(float distance) {
    distance_ = std::max(0.1f, distance);
    update_matrices();
}

void Camera::set_angles(float yaw, float pitch) {
    yaw_ = yaw;
    pitch_ = std::clamp(pitch, -89.0f, 89.0f);
    update_matrices();
}

void Camera::set_fov(float fov) {
    fov_ = std::clamp(fov, 10.0f, 120.0f);
    update_matrices();
}

void Camera::set_aspect(float aspect) {
    aspect_ = aspect;
    update_matrices();
}

void Camera::set_clip(float near, float far) {
    near_ = near;
    far_ = far;
    update_matrices();
}

void Camera::orbit(float delta_yaw, float delta_pitch) {
    yaw_ += delta_yaw;
    pitch_ = std::clamp(pitch_ + delta_pitch, -89.0f, 89.0f);
    update_matrices();
}

void Camera::pan(float delta_x, float delta_y) {
    // Calculate right and up vectors
    glm::vec3 forward = glm::normalize(target_ - position_);
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
    glm::vec3 up = glm::normalize(glm::cross(right, forward));
    
    target_ += right * delta_x * distance_ * 0.1f;
    target_ += up * delta_y * distance_ * 0.1f;
    update_matrices();
}

void Camera::zoom(float delta) {
    distance_ = std::max(0.1f, distance_ - delta * distance_ * 0.1f);
    update_matrices();
}

void Camera::reset() {
    target_ = glm::vec3(0.0f);
    distance_ = 5.0f;
    yaw_ = 45.0f;
    pitch_ = 30.0f;
    update_matrices();
}

glm::vec3 Camera::position() const {
    return position_;
}

glm::mat4 Camera::view_matrix() const {
    return view_matrix_;
}

glm::mat4 Camera::projection_matrix() const {
    return projection_matrix_;
}

glm::mat4 Camera::view_projection_matrix() const {
    return projection_matrix_ * view_matrix_;
}

void Camera::update_matrices() {
    // Calculate camera position from spherical coordinates
    float pitch_rad = glm::radians(pitch_);
    float yaw_rad = glm::radians(yaw_);
    
    position_.x = target_.x + distance_ * cos(pitch_rad) * sin(yaw_rad);
    position_.y = target_.y + distance_ * sin(pitch_rad);
    position_.z = target_.z + distance_ * cos(pitch_rad) * cos(yaw_rad);
    
    // Calculate view matrix
    view_matrix_ = glm::lookAt(position_, target_, glm::vec3(0.0f, 1.0f, 0.0f));
    
    // Calculate projection matrix
    projection_matrix_ = glm::perspective(glm::radians(fov_), aspect_, near_, far_);
}

} // namespace enfusion
