# Tiny Rails routes fixture for quack_from_rails extraction tests.
# Mirrors fastapi_mini: GET articles/:slug + POST login under /api.
Rails.application.routes.draw do
  scope :api do
    get 'articles/:slug', to: 'articles#show'
    post 'login', to: 'sessions#create'
  end
end
