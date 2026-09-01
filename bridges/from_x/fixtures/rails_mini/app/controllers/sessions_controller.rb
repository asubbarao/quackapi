# Strong-params companion for User (login body).
class SessionsController < ApplicationController
  def create
    user_params
    head :created
  end

  private

  def user_params
    params.require(:user).permit(:email, :password, :remember_me)
  end
end
