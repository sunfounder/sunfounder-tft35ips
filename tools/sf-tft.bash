# bash completion for sf-tft
_sf_tft() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"
    if [ "$COMP_CWORD" -eq 1 ]; then
        COMPREPLY=( $(compgen -W "doctor status calibrate cali-test uninstall" -- "$cur") )
        return
    fi
    case "$prev" in
        doctor) COMPREPLY=( $(compgen -W "--fix" -- "$cur") ) ;;
    esac
}
complete -F _sf_tft sf-tft
