/**
 * Tiny Express app fixture for quack_from_express extraction tests.
 * Mirrors fastapi_mini: GET /articles/:slug + POST /login, one model with
 * required + defaulted fields.
 */
import express, { Request, Response, Router } from 'express';

const app = express();
const router = Router();

class UserInLogin {
  email: string;
  password: string;
  remember_me: boolean = false;

  constructor(email: string, password: string, remember_me: boolean = false) {
    this.email = email;
    this.password = password;
    this.remember_me = remember_me;
  }
}

function getArticle(req: Request, res: Response) {
  res.json({ slug: req.params.slug });
}

function login(req: Request, res: Response) {
  const user = req.body as UserInLogin;
  res.status(201).json({ handler: 'login', email: user.email });
}

router.get('/articles/:slug', getArticle);
router.post('/login', login);

app.use('/api', router);

export { app, UserInLogin };
