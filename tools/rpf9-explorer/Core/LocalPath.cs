namespace RpfPreview.Core;

public static class LocalPath
{
    public const FileAttributes RemoteAttributes = FileAttributes.ReparsePoint | FileAttributes.Offline;

    public static bool FileExists(string path)
    {
        try { return File.Exists(Require(path)); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException) { return false; }
    }

    public static string Require(string path)
    {
        if (path.StartsWith(@"\\", StringComparison.Ordinal) || path.StartsWith("//", StringComparison.Ordinal)
            || path.Contains("://", StringComparison.Ordinal))
            throw new IOException("Choose a path on a local drive.");

        string fullPath = Path.GetFullPath(path);
        string root = Path.GetPathRoot(fullPath)!;
        if (OperatingSystem.IsWindows()
            && (root.Length != 3 || root[1] != ':' || fullPath.AsSpan(2).Contains(':')
                || new DriveInfo(root).DriveType == DriveType.Network))
            throw new IOException("Choose a path on a local drive.");

        var segments = new Stack<string>();
        for (string? part = Path.TrimEndingDirectorySeparator(fullPath); part != null; part = Path.GetDirectoryName(part))
            segments.Push(part);
        while (segments.TryPop(out string? part))
        {
            try
            {
                if ((File.GetAttributes(part) & RemoteAttributes) != 0)
                    throw new IOException("Choose a local path without links, junctions, or offline files.");
            }
            catch (FileNotFoundException) { break; }
            catch (DirectoryNotFoundException) { break; }
        }
        return fullPath;
    }
}
