#!/usr/bin/env bash

#	Install the linux kernel and the service running the boottime test

set -euxo pipefail

# The settings of boottime_test_record.sh go into this file, which the
# service reads (EnvironmentFile=).
SETTINGS=/etc/default/boottime_test_record

# The value of $1 in the file $2 as the service reads it with EnvironmentFile=,
# for the simple KEY=VALUE lines this file holds: blank lines and those
# starting with # or ; are skipped, a CR line end and the blanks around key
# and value are dropped, and then quotes around the whole value. A # after the
# value is no comment to systemd either, and stays part of it. The last
# assignment wins. Nothing is printed without one.
#
# Only those simple lines, in the whole file. systemd also joins a line ending
# in a backslash with the next one, takes a backslash in a value as an escape,
# starts a quoted part at any quote in a value and carries it across line ends
# up to the closing one, and ends a line at a CR as much as at a newline -
# none of which this repeats. So a file that uses any of it, in any line,
# fails with status 2, rather than have its value read otherwise than the
# service reads it: another key's quote left open, or a line split at a CR,
# hides a GETRAWENTROPY line the service reads as part of that key's value,
# or shows one it does not read at all. Refused are a line ending in a
# backslash, a CR before the line end, and outside comments any backslash or
# quote but one pair around a whole value.
settings_value()
{
	local line key val found=""

	while IFS= read -r line || [ -n "$line" ]
	do
		line=${line%$'\r'}
		case $line in
		*\\)
			echo "$2: a line ends in a backslash, which continues it to systemd" >&2
			return 2 ;;
		*$'\r'*)
			echo "$2: a line holds a CR, which ends it there to systemd" >&2
			return 2 ;;
		esac
		line=${line#"${line%%[![:space:]]*}"}
		case $line in
		''|'#'*|';'*) continue ;;
		*=*) ;;
		*[\\\"\']*)
			echo "$2: a line without = holds a backslash or a quote; write only plain KEY=value lines" >&2
			return 2 ;;
		*) continue ;;
		esac

		key=${line%%=*}
		key=${key%"${key##*[![:space:]]}"}

		val=${line#*=}
		val=${val#"${val%%[![:space:]]*}"}
		val=${val%"${val##*[![:space:]]}"}
		case $val in
		\"*\") val=${val#\"}; val=${val%\"} ;;
		\'*\') val=${val#\'}; val=${val%\'} ;;
		esac
		case $key$val in
		*[\\\"\']*)
			echo "$2: $key holds a backslash or a quote not around the whole value; write it as plain $key=value" >&2
			return 2 ;;
		esac
		[ "$key" = "$1" ] && found=$val
	done < "$2"

	if [ -n "$found" ]
	then
		printf '%s\n' "$found"
	fi
}

# The recording tool the service will run: GETRAWENTROPY from $SETTINGS, else
# the script's default. One given here in the environment reaches the service
# only through $SETTINGS, so both are checked: the one given, and the one the
# service will actually run.
configured=""
if [ -f "$SETTINGS" ]
then
	configured=$(settings_value GETRAWENTROPY "$SETTINGS") || exit 1
fi
configured=${configured:-/usr/local/sbin/getrawentropy}
GETRAWENTROPY=${GETRAWENTROPY:-$configured}

# Absolute paths only: the service runs from /, and the script looks a bare
# name up in its PATH, so neither would be the file checked here, relative to
# wherever the installer runs.
for path in "$GETRAWENTROPY" "$configured"
do
	case $path in
	/*) ;;
	*)
		echo "GETRAWENTROPY must be an absolute path, not $path"
		exit 1 ;;
	esac
done

if [ ! -x "$GETRAWENTROPY" ]
then
	echo "getrawentropy must be installed as $GETRAWENTROPY"
	echo "(or set GETRAWENTROPY in $SETTINGS to where it is)"
	exit 1
fi

if [ "$GETRAWENTROPY" != "$configured" ]
then
	if [ ! -x "$configured" ]
	then
		echo "The service runs $configured, which is not installed:"
		echo "set GETRAWENTROPY=$GETRAWENTROPY in $SETTINGS"
		exit 1
	fi
	echo "Note: the service runs $configured; set GETRAWENTROPY=$GETRAWENTROPY in $SETTINGS"
fi

if ! { cp boottime_test_record.sh /usr/local/sbin/ &&
       chmod u+x /usr/local/sbin/boottime_test_record.sh &&
       cp boottime_test_record.service /etc/systemd/system/; }
then
	echo "Installing the script and the service failed"
	exit 1
fi

# The copies take the SELinux labels of where they come from, which the
# service may not run with - Fedora needs this. Systems without SELinux have
# no restorecon, or have it with SELinux disabled; they need nothing.
if command -v restorecon >/dev/null 2>&1 &&
   { ! command -v selinuxenabled >/dev/null 2>&1 || selinuxenabled; }
then
	if ! restorecon -v /usr/local/sbin/boottime_test_record.sh \
			/etc/systemd/system/boottime_test_record.service \
			"$configured"
	then
		echo "restorecon of the installed files failed"
		exit 1
	fi
fi

if ! systemctl enable boottime_test_record
then
	echo "Enabling the service failed"
	exit 1
fi

echo "Settings of the test (GETRAWENTROPY, DEBUGFS_FILE, RAW_OPTS, KCAPI_NAME,"
echo "TESTS, ...; see boottime_test_record.sh) go into $SETTINGS."
echo "To improve the reboot speed change the timeout in /etc/default/grub as follows"
echo "GRUB_TIMEOUT=1"
echo "Then run grub2-mkconfig to update the grub configuration file for the new setting to take effect."
