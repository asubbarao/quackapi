// Tiny Gin app fixture for quack_from_gin extraction tests.
// Mirrors fastapi_mini: GET /articles/:slug + POST /login under /api,
// one struct with required + optional/defaulted-ish fields.
package main

import (
	"net/http"

	"github.com/gin-gonic/gin"
)

type UserInLogin struct {
	Email      string `json:"email" binding:"required"`
	Password   string `json:"password" binding:"required"`
	RememberMe bool   `json:"remember_me,omitempty"`
}

func GetArticle(c *gin.Context) {
	c.JSON(http.StatusOK, gin.H{"slug": c.Param("slug")})
}

func Login(c *gin.Context) {
	var user UserInLogin
	if err := c.ShouldBindJSON(&user); err != nil {
		c.JSON(http.StatusUnprocessableEntity, gin.H{"error": err.Error()})
		return
	}
	c.JSON(http.StatusCreated, gin.H{"handler": "login", "email": user.Email})
}

func main() {
	r := gin.Default()
	api := r.Group("/api")
	{
		api.GET("/articles/:slug", GetArticle)
		api.POST("/login", Login)
	}
	_ = r.Run(":8080")
}
