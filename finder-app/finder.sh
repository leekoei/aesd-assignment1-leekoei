#!/bin/sh

# Check if two arguments are provided
if [ "$#" -ne 2 ]; then
    echo "Error: exactly two arguments are required."
    echo "Usage: $0 <directory> <search_string>"
    exit 1
fi

filesdir=$1
searchstr=$2

# Check if filesdir exists and is a directory
if [ ! -d ${filesdir} ] ; then
    echo "Error: Directory $filesdir does not exist."
    exit 1
fi

# Count the number of files and matching lines
X=`ls ${filesdir} | wc -l`
Y=`grep -r ${searchstr} ${filesdir} | wc -l`

echo "The number of files are $X and the number of matching lines are $Y"
