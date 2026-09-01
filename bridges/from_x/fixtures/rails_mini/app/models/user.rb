# Tiny ActiveModel-style model for quack_from_rails_models.
# email + password required (presence); remember_me optional (no presence).
class User < ApplicationRecord
  validates :email, presence: true
  validates :password, presence: true
  validates :remember_me, inclusion: { in: [true, false] }, allow_blank: true
end
