# bash completion for omaword                               -*- shell-script -*-
# `omaword completion bash` prints it; bin/install installs it.

_omaword()
{
    local cur=${COMP_WORDS[COMP_CWORD]} prev=${COMP_WORDS[COMP_CWORD-1]}
    local commands="open lookup quote search translations link import completion help"
    case $prev in
        -t | --translation)
            COMPREPLY=($(compgen -W "$(omaword translations --names 2>/dev/null)" -- "$cur"))
            return ;;
        --color)
            COMPREPLY=($(compgen -W "auto always never" -- "$cur"))
            return ;;
        --limit)
            return ;;
        --import | import)
            compopt -o filenames 2>/dev/null
            COMPREPLY=($(compgen -f -- "$cur"))
            return ;;
    esac
    if ((COMP_CWORD == 1)); then
        COMPREPLY=($(compgen -W "$commands --help --version" -- "$cur"))
        return
    fi
    local options
    case ${COMP_WORDS[1]} in
        open) options="--here --translation --help" ;;
        lookup) options="--help" ;;
        quote) options="--plain --json --translation --help" ;;
        search) options="--all --limit --json --color --translation --help" ;;
        translations | versions) options="--names --json --help" ;;
        link) options="--translation --help" ;;
        completion) ((COMP_CWORD == 2)) && options="bash zsh fish" ;;
        help) ((COMP_CWORD == 2)) && options="$commands" ;;
    esac
    [[ $cur == -* || ${COMP_WORDS[1]} == completion || ${COMP_WORDS[1]} == help ]] &&
        COMPREPLY=($(compgen -W "$options" -- "$cur"))
}
complete -F _omaword omaword
