# bash completion for omaword                               -*- shell-script -*-
# `omaword completion bash` prints it; bin/install installs it.

# A group's commands.
_omaword_subcommands()
{
    case $1 in
        translations | versions) echo "list" ;;
    esac
}

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
        --in)
            COMPREPLY=($(compgen -W "ot nt apocrypha" -- "$cur"))
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
    local command=${COMP_WORDS[1]} subcommands
    subcommands=$(_omaword_subcommands "$command")
    if [[ -n $subcommands ]]; then
        if ((COMP_CWORD == 2)) && [[ $cur != -* ]]; then
            COMPREPLY=($(compgen -W "$subcommands" -- "$cur"))
            return
        fi
        # The group's default command (listed first), unless another is named.
        if [[ " $subcommands " == *" ${COMP_WORDS[2]} "* ]]; then command+=" ${COMP_WORDS[2]}"; else command+=" ${subcommands%% *}"; fi
    fi
    local options
    case $command in
        open) options="--here --translation --help" ;;
        lookup) options="--translation --help" ;;
        quote) options="--plain --json --translation --help" ;;
        search) options="--all --in --limit --json --color --translation --help" ;;
        "translations list" | "versions list") options="--names --json --help" ;;
        link) options="--translation --help" ;;
        completion) ((COMP_CWORD == 2)) && options="bash zsh fish" ;;
        help) ((COMP_CWORD == 2)) && options="$commands"
              ((COMP_CWORD == 3)) && options=$(_omaword_subcommands "${COMP_WORDS[2]}") ;;
    esac
    [[ $cur == -* || ${COMP_WORDS[1]} == completion || ${COMP_WORDS[1]} == help ]] &&
        COMPREPLY=($(compgen -W "$options" -- "$cur"))
}
complete -F _omaword omaword
