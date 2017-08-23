# Postfix MTA - High-performance mail transport agent

This project is a fork of the git repository of postfix maintained by
Viktor Dukhovni at https://github.com/vdukhovni/postfix. Upstream changes
will be merged from this repository.

This docker image builds postfix from source and therefor is not based
on postfix packages of a linux distribution. It tracks upstream versions.

This means that 'latest' is really the latest development release
of postfix.

It is recommented to use one of the release branches:

 * 3.2-latest
 * 3.1-latest
 * 3.0-latest
 * 2.11-latest

Which track the release branches of postfix.

There are also tagged builds for explicit release like '3.2.2'.
Use them if you want to stick to a certain version.

## Usage Examples

This will just start the latest postfix images:

```
$ docker run -it --rm benningm/postfix:3.2-latest
```

In most cases you want to at least specifiy a persistent volume for the mail queue
and your own postfix configuration:

```
$ docker run -it --rm \
  -v /etc/postfix/main.cf:/etc/postfix/main.cf \
  -v /var/spool/postfix:/var/spool/postfix \
  benningm/postfix:3.2-latest
```

