/*
 * Copyright 2010-2019 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
 */

#ifndef DIALOG_H_HEADER_GUARD
#define DIALOG_H_HEADER_GUARD

namespace bx { class FilePath; class StringView; }

struct FileSelectionDialogType
{
	enum Enum
	{
		Open,
		Save,
		
		Count
	};
};

bool openFileSelectionDialog(
							 bx::FilePath& _inOutFilePath
							 , FileSelectionDialogType::Enum _type
							 , const bx::StringView& _title
							 , const bx::StringView& _filter = "All Files | *"
							 );


#endif // DIALOG_H_HEADER_GUARD
