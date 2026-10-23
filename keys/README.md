# Package signing keys (public halves)

CI signs the package index of each build with these keys when the matching
private key is stored as a repository secret:

| File | Used by | Secret holding the private key |
|---|---|---|
| `af358956e09d0655` | OpenWrt 24.10 (opkg, usign) | `USIGN_KEY` |
| `dawn-dev.pem` | OpenWrt 25.12 (apk, ECDSA P-256) | `APK_KEY` |

Install the public key on a router once, and packages from this repo's feed
install without `--allow-untrusted`:

```sh
# OpenWrt 25.12
wget -O /etc/apk/keys/dawn-dev.pem <raw URL of keys/dawn-dev.pem>   # or scp it
# OpenWrt 24.10
wget -O /etc/opkg/keys/af358956e09d0655 <raw URL of keys/af358956e09d0655>
```

The private keys are not in this repository. To replace them, generate a new
pair, put the private halves in the secrets above and commit the public ones
here:

```sh
usign -G -c "dawn-dev package feed" -s usign.key -p usign.pub   # name the .pub after: usign -F -p usign.pub
openssl ecparam -name prime256v1 -genkey -noout -out apk.key
openssl ec -in apk.key -pubout -out dawn-dev.pem
```
