# LuxEngine documentation site

The Korean, English, Simplified Chinese, and Japanese guides live in `ko/`, `en/`, `zh/`, and `ja/`. Keep the guide filenames shared across languages. The API reference is generated once from the ScriptCore project. The tutorials include the source of the same runnable sample compiled by CI.

Build and serve the site with the .NET 10 SDK from this directory.

```sh
dotnet tool restore
dotnet docfx metadata docfx.json --warningsAsErrors
dotnet docfx build docfx.json --warningsAsErrors
dotnet docfx serve _site
```

Open `http://localhost:8080`. Generated API files and HTML are ignored by Git.

For the first deployment, a repository administrator selects **GitHub Actions** in **Settings → Pages → Build and deployment → Source**. The documentation workflow builds on pull requests and deploys from `main`. After enabling Pages, run **Documentation → Run workflow** on `main` if the changes have already been merged.
