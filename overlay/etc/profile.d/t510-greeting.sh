# t510-greeting for login shells on a terminal: the console, ssh and
# xfce4-terminal. Not for scripts, not for sudo -i; ~/.hushlogin hides it.
case $- in
*i*) ;;
*) return ;;
esac
if [ -t 1 ] && [ ! -e "$HOME/.hushlogin" ] && [ -z "$SUDO_USER" ]; then
	t510-greeting
fi
