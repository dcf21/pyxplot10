// help.c
//
// The code in this file is part of Pyxplot
// <http://www.pyxplot.org.uk>
//
// Copyright (C) 2006-2026 Dominic Ford <coders@pyxplot.org.uk>
//               2008-2013 Ross Church
//
// $Id$
//
// Pyxplot is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
// version.
//
// You should have received a copy of the GNU General Public License along with
// Pyxplot; if not, write to the Free Software Foundation, Inc., 51 Franklin
// Street, Fifth Floor, Boston, MA  02110-1301, USA

// ----------------------------------------------------------------------------

// Implements the help command. The help pages are held in the XML document help.xml, in which each element is a topic
// and nested elements are subtopics. The words of the user's request are matched against the element names, allowing
// abbreviations, and the matching page is word-wrapped and shown through the user's pager.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "coreUtils/dict.h"
#include "coreUtils/errorReport.h"
#include "coreUtils/list.h"
#include "coreUtils/memAlloc.h"
#include "coreUtils/stringList.h"

#include "parser/cmdList.h"
#include "parser/parser.h"

#include "userspace/context.h"

#include "pplConstants.h"

#define MAX_HELP_DEPTH 16 // The maximum possible depth of a help hierarchy
#define MAX_HELP_HITS  12 // The maximum possible number of help topics which match the same autocomplete strings

//! help_PagerDisplay - Display a page of help text, through the user's pager if the session is interactive
//! \param [in] c - The error-reporting context
//! \param [in] PageName - The name of the help topic, displayed as a heading
//! \param [in] node - The XML node containing the help text
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal

static void help_PagerDisplay(pplerr_context *c, const char *PageName, const xmlNode *node, const int interactive) {
    // The page is built up in textBuffer, using the "\\" line-break markers understood by ppl_strWordWrap
    char textBuffer[LSTR_LENGTH];
    char textBuffer2[LSTR_LENGTH];
    char defaultpagerName[] = "less";
    const char *pagerName;
    int Ncolumns;
    FILE *pagerHandle;

    // Start with a title line, then copy the text of the page
    ppl_snprintfTrunc(textBuffer, sizeof(textBuffer), "\\\\**** Help Topic: %s****\\\\", PageName);
    int i = strlen(textBuffer);
    for (xmlNode *cur_node = node->children; cur_node; cur_node = cur_node->next) // Loop over children
        // Copy each text node, substituting the version number and date for $VERSION and $DATE
        if (cur_node->type == XML_TEXT_NODE) {
            const int strlen_version = strlen("$VERSION");
            const int strlen_date = strlen("$DATE");
            // Copy the text character by character
            for (int j = 0; cur_node->content[j] != '\0';) {
                // Substitute the placeholders where they appear
                if (strncmp((char *) cur_node->content + j, "$VERSION", strlen_version) == 0) {
                    j += strlen_version;
                    strcpy(textBuffer + i, VERSION);
                    i += strlen(textBuffer + i);
                } else if (strncmp((char *) cur_node->content + j, "$DATE", strlen_date) == 0) {
                    j += strlen_date;
                    strcpy(textBuffer + i, DATE);
                    i += strlen(textBuffer + i);
                } else {
                    textBuffer[i++] = cur_node->content[j++];
                }
            }
        } // NB, textBuffer is not null terminated at this point, but we're about to add stuff to it...

    // End the page text with a blank line
    sprintf(textBuffer + i, "\\\\\\\\\\\\");
    // Insert information about children
    int Nchildren = 0;
    for (xmlNode *cur_node = node->children; cur_node; cur_node = cur_node->next) // Loop over children
        // Each child element is a subtopic; list their names, separated by commas
        if (cur_node->type == XML_ELEMENT_NODE) {
            // Introduce the list before the first subtopic
            if (Nchildren == 0) {
                sprintf(textBuffer + i, "This help page has the following subtopics:\\\\\\\\");
                i += strlen(textBuffer + i);
            } else {
                sprintf(textBuffer + i, ", ");
                i += strlen(textBuffer + i);
            }
            sprintf(textBuffer + i, "%s", cur_node->name);
            i += strlen(textBuffer + i);
            Nchildren++;
        }
    // End the list of subtopics, or say there are none
    if (Nchildren == 0) {
        sprintf(textBuffer + i, "This help page has no subtopics.\\\\\\\\");
        i += strlen(textBuffer + i);
    } else {
        sprintf(textBuffer + i, ".\\\\\\\\");
        i += strlen(textBuffer + i);
    }

    // If interactive, tell user how to quit, and also work out column width of display
    if (interactive != 0) {
        sprintf(textBuffer + i, "Press the 'Q' key to exit this help page.\\\\");
        const char *Ncolumns_text = getenv("COLUMNS");
        if (!((Ncolumns_text != NULL) && (Ncolumns = ppl_getFloat(Ncolumns_text, &i), i == strlen(Ncolumns_text))))
            Ncolumns = 80;
        pagerName = getenv("PAGER");
        if (pagerName == NULL) pagerName = defaultpagerName;
    } else {
        Ncolumns = 80;
        pagerName = defaultpagerName;
    }

    // Word wrap the text we have, and send it to a pager
    ppl_strWordWrap(textBuffer, textBuffer2, Ncolumns);
    // Display the page
    if (interactive == 0) {
        ppl_report(c, textBuffer2); // In non-interactive sessions, we just send text to stdout
    } else {
        // Pipe the page into the pager
        if ((pagerHandle = popen(pagerName, "w")) == NULL) {
            ppl_error(c, ERR_INTERNAL, -1, -1, "Cannot open pipe to pager application.");
        } else {
            fprintf(pagerHandle, "%s", textBuffer2);
            pclose(pagerHandle);
        }
    }
    return;
}

//! help_TopicPrint - Write the name of a help topic, made up of a list of words
//! \param [out] out - Character buffer into which to write the topic name
//! \param [in] words - Array of the words making up the topic name
//! \param [in] Nwords - The number of words in <words>

static void help_TopicPrint(char *out, char **words, const int Nwords) {
    int i, j;
    // Join the words with spaces
    for (i = 0, j = 0; i <= Nwords; i++) {
        strcpy(out + j, words[i]);
        j += strlen(out + j);
        out[j++] = ' ';
    }
    out[j] = '\0';
}

//! help_MatchFound - Record that a node in the help XML document matches the user's request. If more than one
//! node matches, and neither is an abbreviation of the other, the request is flagged as ambiguous.
//! \param [in] c - The error-reporting context
//! \param [in] node - The XML node which matched
//! \param [in|out] matchNode - Pointer to the best match found so far
//! \param [in] topicWords - The words of the user's help request
//! \param [in|out] matchTextPos - The position in <helpTexts> of the best match found so far
//! \param [in|out] Nmatches - The number of matches found so far
//! \param [in|out] ambiguous - Set to one if the request matches more than one topic
//! \param [in] helpPosition - The names of the nodes on the path from the root of the XML document to <node>
//! \param [in|out] helpTexts - Array of the names of all the topics which have matched

static void help_MatchFound(pplerr_context *c, xmlNode *node, xmlNode **matchNode, const list *topicWords, int *matchTextPos,
                            int *Nmatches, int *ambiguous, char **helpPosition, char **helpTexts) {

    // There is a fixed limit on the number of matches which can be recorded
    if ((*Nmatches) == MAX_HELP_HITS) {
        ppl_error(c, ERR_INTERNAL, -1, -1, "Cannot parse ppl_help.xml. Need to increase MAX_HELP_HITS.");
        return;
    }
    help_TopicPrint(helpTexts[*Nmatches], helpPosition, ppl_listLen(topicWords));
    // Unless we already know the request is ambiguous, decide whether this match or the previous one is better
    if (*ambiguous == 0) {
        // The first match is the best so far
        if (*matchNode == NULL) {
            *matchTextPos = 0;
            *matchNode = node; // This is the first match we've found
        } else {
            const list *prevMatchPosition = ppl_strSplit(helpTexts[*matchTextPos]);

            int abbreviation = 1; // If previous match is an autocomplete shortening of current match, let it stand
            for (int i = 0; i <= ppl_listLen(topicWords); i++)
                // Stop if any word of the previous match is not an abbreviation of this one
                if (ppl_strAutocomplete((char *) ppl_listGetItem(prevMatchPosition, i), helpPosition[i], 1) == -1) {
                    abbreviation = 0;
                    break;
                }
            // Otherwise, check whether this match is an abbreviation of the previous one
            if (abbreviation != 1) {
                abbreviation =
                        1; // If current match is an autocomplete shortening of previous match, the current match wins
                for (int i = 0; i <= ppl_listLen(topicWords); i++)
                    // Stop if any word of this match is not an abbreviation of the previous one
                    if (ppl_strAutocomplete(helpPosition[i], (char *) ppl_listGetItem(prevMatchPosition, i), 1) == -1) {
                        abbreviation = 0;
                        break;
                    }
                // This match wins if it is an abbreviation of the previous one; otherwise neither wins
                if (abbreviation == 1) {
                    *matchTextPos = *Nmatches;
                    *matchNode = node;
                } else {
                    *ambiguous = 1; // We have multiple ambiguous matches
                }
            }
        }
    }
    (*Nmatches)++;
    return;
}

//! help_explore - Recursively search the help XML document for topics matching the user's request
//! \param [in] c - The error-reporting context
//! \param [in] node - The XML node at which to start searching
//! \param [in|out] matchNode - Pointer to the best match found so far
//! \param [in] topicWords - The words of the user's help request
//! \param [in|out] matchTextPos - The position in <helpTexts> of the best match found so far
//! \param [in|out] Nmatches - The number of matches found so far
//! \param [in|out] ambiguous - Set to one if the request matches more than one topic
//! \param [in|out] helpPosition - The names of the nodes on the path from the root of the XML document to <node>
//! \param [in|out] helpTexts - Array of the names of all the topics which have matched
//! \param [in] depth - The depth of <node> within the XML document

void help_explore(pplerr_context *c, xmlNode *node, xmlNode **matchNode, list *topicWords, int *matchTextPos,
                  int *Nmatches, int *ambiguous, char **helpPosition, char **helpTexts, const int depth) {
    xmlNode *cur_node = NULL;

    // Do not search deeper than the number of words in the request
    if (depth > ppl_listLen(topicWords)) return;
    // There is a fixed limit on the depth of the help document
    if (depth == MAX_HELP_DEPTH) {
        ppl_error(c, ERR_INTERNAL, -1, -1, "Cannot parse ppl_help.xml. Need to increase MAX_HELP_DEPTH.");
        return;
    }

    for (cur_node = node; cur_node; cur_node = cur_node->next) // Loop over siblings
    {
        int match = 1;
        // Record this element's name, and check whether each word of the request so far abbreviates the element names
        // on its path
        if (cur_node->type == XML_ELEMENT_NODE) {
            sprintf(helpPosition[depth], "%s", cur_node->name); // Converted signedness of chars
            for (int i = 1; i <= depth; i++)
                // This element does not match the request
                if (ppl_strAutocomplete((char *) ppl_listGetItem(topicWords, i - 1), helpPosition[i], 1) == -1) {
                    match = 0;
                    break;
                }
            if ((match == 1) && (depth == ppl_listLen(topicWords)))
                help_MatchFound(c, cur_node, matchNode, topicWords, matchTextPos, Nmatches, ambiguous, helpPosition,
                                helpTexts);
        }
        if (match == 1)
            help_explore(c, cur_node->children, matchNode, topicWords, matchTextPos, Nmatches, ambiguous, helpPosition,
                         helpTexts, depth + 1);
    }
    return;
}

//! ppl_directive_help - Implement the help command, which displays help on a topic from Pyxplot's help XML document
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal

void ppl_directive_help(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive) {
    // The variables used to search the help document
    char msg[PPL_MSG_LENGTH];
    pplerr_context *e = &c->errcontext;
    list *topicWords = NULL; // A list of the help topic words supplied by the user
    char *topicString = NULL; // The string of help topic words supplied by the user
    char filename[FNAME_LENGTH]; // The filename of ppl_help.xml
    char *helpPosition
            [MAX_HELP_DEPTH]; // A list of xml tags, used to keep track of our position as we traverse the xml hierarchy
    char *helpTexts[MAX_HELP_HITS]; // A list of all of the help topics which have matched the user's request
    xmlDoc *doc = NULL; // The XML document ppl_help.xml
    xmlNode *root_element = NULL; // The root element of the above
    xmlNode *matchNode = NULL; // The XML node which best fits the user's request
    int matchTextPos = -1; // The position within helpTexts of the node pointed to by matchNode
    int ambiguous = 0; // Becomes one if we find that the user's request matches multiple help pages
    int Nmatches = 0; // Counts the number of pages the user's request matches
    int i;

    // Split the user's request into words
    topicString = (char *) in->stk[PARSE_help_topic].auxil;
    topicWords = ppl_strSplit(topicString);

    ppl_snprintfTrunc(filename, sizeof(filename), "%s%s%s", SRCDIR, PATHLINK, "help.xml"); // Find help.xml
    LIBXML_TEST_VERSION

    // Load the help document
    doc = xmlReadFile(filename, NULL, 0);

    // Report an error if the help document cannot be read
    if (doc == NULL) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Help command cannot find help data in expected location of '%s'.",
                          filename);
        ppl_error(e, ERR_INTERNAL, -1, -1, msg);
        return;
    }

    // Allocate buffers for the path of the current position in the document, and for the names of the topics found
    for (i = 0; i < MAX_HELP_DEPTH; i++) helpPosition[i] = NULL;
    for (i = 0; i < MAX_HELP_HITS; i++) helpTexts[i] = NULL;
    for (i = 0; i < MAX_HELP_DEPTH; i++)
        if ((helpPosition[i] = (char *) malloc(SSTR_LENGTH * sizeof(char))) == NULL) goto cleanup;
    for (i = 0; i < MAX_HELP_HITS; i++)
        if ((helpTexts[i] = (char *) malloc(SSTR_LENGTH * sizeof(char))) == NULL) goto cleanup;

    // Search the document for topics matching the request
    root_element = xmlDocGetRootElement(doc);
    help_explore(e, root_element, &matchNode, topicWords, &matchTextPos, &Nmatches, &ambiguous, helpPosition, helpTexts,
                 0);

    // Show the page which matched, or report that the request was ambiguous or matched nothing
    if (ambiguous == 1) {
        ppl_report(e, "Ambiguous help request. The following help topics were matched:");
        for (i = 0; i < Nmatches; i++) ppl_report(e, helpTexts[i]);
        ppl_report(e, "Please make your help request more specific, and try again.");
    } else if (matchNode == NULL) {
        ppl_report(e, "Please make your help request more specific, and try again.");
    } else {
        help_PagerDisplay(e, helpTexts[matchTextPos], matchNode, interactive);
    }

// Free the buffers and the document
cleanup:
    // Free the path buffers
    for (i = 0; i < MAX_HELP_DEPTH; i++) {
        free(helpPosition[i]);
        helpPosition[i] = NULL;
    }
    // Free the names of the topics found
    for (i = 0; i < MAX_HELP_HITS; i++) {
        free(helpTexts[i]);
        helpTexts[i] = NULL;
    }
    xmlFreeDoc(doc);
    doc = NULL; // Tidy up
    xmlCleanupParser();
    return;
}
