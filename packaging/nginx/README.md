<!-- Created for Goblin Skiff on 2026-09-19. -->
# Goblin Skiff website on hail

The public site is <https://skiff.goblinreactor.com/>. Hail's nginx virtual
host proxies to `http://192.168.1.11:8888/skiff.goblinreactor.com/`.
That backend serves the same NFS share mounted on hail at
`/mnt/distribution`; the site's directory is
`/mnt/distribution/skiff.goblinreactor.com`.

The installed virtual host is
`/etc/nginx/sites-available/skiff.goblinreactor.com`, enabled through
`/etc/nginx/sites-enabled/skiff.goblinreactor.com`. Its reviewed source is
[skiff.goblinreactor.com.conf](skiff.goblinreactor.com.conf).

Publish `html/index.html`, `html/goblin.png` and `TRADEMARKS.md` in that
directory. The page declares the Skiff hostname in its canonical and social
metadata and links the trademark notice. Preserve the image and upstream
attribution when updating the page.

After changing the virtual host, run `sudo nginx -t` before
`sudo systemctl reload nginx`. A graceful reload can return before the new
workers accept requests, so poll the new Host route briefly before deciding
whether the deployment failed. Verify the backend directory, nginx's local
Host route, and the public HTTPS page and assets separately.

The September 19 hostname cutover passed those checks. The previous virtual
host and public directory were removed after verification. Private rollback
copies and the deployment manifest are retained on hail under
`/var/tmp/goblin-skiff-site-20260919`.
